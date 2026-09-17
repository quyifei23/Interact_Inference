#include "worker_common.cuh"
#include "recovery.h"
#include <memory>
#include <sys/prctl.h>
int main(int argc,char** argv){
    std::unique_ptr<ap::Mapping> mapping;std::unique_ptr<ap::GpuWorker> gpu;std::unique_ptr<ap::RmControl> rm;
    std::unique_ptr<ap::OwnedGroup> group;
    bool prepared=false,work_in_flight=false,group_request_attempted=false;
    ap::Recovery recovery;std::string run_dir;
    auto cleanup=[&](){
        ap::save_control_journal(); // persist evidence before potentially blocking cleanup
        ap::stop_requested=0;
        std::ofstream f(run_dir+"/bg_recovery.txt",std::ios::app);
        if(group)f<<"NO_PERSISTENT_SCHEDULING_CONFIGURATION_CHANGED; PREEMPT has no hold/resume API\n";
        bool restored=!rm||recovery.restore(*rm,f);f<<(restored?"CONFIGURATION_RESTORED":"RECOVERY_FAILED")<<std::endl;
        ap::save_control_journal();ap::save_rm_observation(run_dir,"bg_");return restored;
    };
    try{
        auto o=ap::parse(argc,argv);run_dir=o.run_dir;auto plan=ap::mode_plan(o.mode);
        ap::configure_rm(plan.group_identity?ap::RmStage::GroupActive:(plan.identity?ap::RmStage::Active:ap::RmStage::Disabled),o.test_host,ap::GroupOwner::Background);
        std::signal(SIGTERM,ap::on_stop);std::signal(SIGINT,ap::on_stop);
        if(o.shared_path.empty())throw std::runtime_error("bg_worker needs controller shared mapping");
        mapping=std::make_unique<ap::Mapping>(o.shared_path.c_str());auto& s=*mapping->shared;
        if(prctl(PR_SET_PDEATHSIG,SIGTERM)!=0)throw std::runtime_error("PR_SET_PDEATHSIG failed");
        if(getppid()!=pid_t(s.host.controller_pid))throw std::runtime_error("Controller exited before BG startup");
        ap::open_control_journal(run_dir,"bg");
        gpu=std::make_unique<ap::GpuWorker>(s.bg,o.bg_us,o.waves,o.heartbeat_ns,o.graph,o.bg_iterations,o.diagnostic);
        gpu->cleanup_log=run_dir+"/bg_cuda_cleanup.log";
        ap::note_cuda_ready(gpu->prop.major,gpu->prop.minor);
        gpu->calibrate_observation(run_dir+"/bg_calibration_before.csv");
        if((o.mode=="none"||plan.group_identity)&&!o.graph&&!o.diagnostic)gpu->prepare_short_reference();
        std::ofstream(run_dir+"/bg_capture.txt")<<ap::capture_inventory();
        if(plan.identity)rm=std::make_unique<ap::RmControl>(ap::discover_owned_compute_group());
        if(plan.group_identity)group=std::make_unique<ap::OwnedGroup>(ap::bind_worker_group(*gpu,run_dir,"bg"));
        std::ofstream identity(run_dir+"/bg_identity.txt");ap::write_identity(identity,*gpu,rm.get());
        if(group)ap::write_group_identity(identity,*group);
        if(plan.timeslice){
            ap::require_ok(rm->get_timeslice(recovery.original_timeslice),"BG GET_TIMESLICE");
            recovery.timeslice=true;ap::require_ok(rm->timeslice(o.timeslice_us),"BG SET_TIMESLICE");
            uint64_t readback=0;auto r=rm->get_timeslice(readback);
            identity<<"timeslice_requested_us="<<o.timeslice_us<<" readback_us="<<(r.ok()?std::to_string(readback):"unknown")<<" get_status="<<r.describe()<<" hardware_quantum_us=unknown\n";
        }
        if(rm){auto r=rm->get_preemption_mode(s.host.bg_mode_before);s.host.bg_mode_status_before=r.rm_status;identity<<"mode_before="<<s.host.bg_mode_before<<" status="<<r.describe()<<'\n';}
        s.host.bg_pid=getpid();s.host.bg_context=reinterpret_cast<uintptr_t>(gpu->context);s.host.bg_blocks=gpu->blocks;
        if(rm){s.host.bg_identity_valid=1;s.host.bg_tsg=rm->identity().tsg_id;s.host.bg_client=rm->identity().client;s.host.bg_group=rm->identity().group;s.host.bg_engine=rm->identity().engine;}
        if(group){const auto& b=group->identity.binding();s.host.bg_group_identity_valid=1;s.host.bg_tsg=*group->info.hardware_tsg_id;
            s.host.bg_client=b.client;s.host.bg_group=b.group.token.handle;s.host.bg_engine=b.group.engine;}
        s.host.bg_iterations=gpu->iterations;s.host.bg_solo_us=gpu->solo_us;s.host.bg_uninstrumented_us=gpu->uninstrumented_us;
        std::strncpy(s.host.bg_uuid,ap::uuid_string(gpu->prop.uuid).c_str(),sizeof(s.host.bg_uuid)-1);identity.flush();ap::save_control_journal();
        ap::release(&s.host.ready,1);uint32_t seen=0;bool running=true;
        while(running){
            uint64_t deadline=ap::monotonic_ns()+60ull*1000000000;
            while(ap::acquire(&s.host.command_seq)==seen){ap::check_stop();if(ap::monotonic_ns()>deadline)throw std::runtime_error("Controller disappeared");ap::relax();}
            s.host.command_received_ns=ap::monotonic_ns();uint32_t seq=ap::acquire(&s.host.command_seq);
            s.host.result={};s.host.preliminary_result={};s.host.bg_done_at_owner_check=2;ap::record_trial(s.host.trial_id);
            auto need_rm=[&](){if(!rm)throw std::runtime_error("OBJECT_BINDING_UNAVAILABLE: command needs RM identity");};
            switch(s.host.command){
            case ap::Command::None:s.host.bg_done_at_owner_check=ap::acquire(&s.bg.done);break;
            case ap::Command::Prepare:
                if(work_in_flight)throw std::runtime_error("BG must drain before telemetry reset");
                if(group&&!ap::verified_group_current(group->identity))throw std::runtime_error("OBJECT_BINDING_UNAVAILABLE: stale BG group before trial");
                gpu->reset();gpu->host->launch_id=s.host.trial_id;s.host.bg_correct=0;prepared=true;break;
            case ap::Command::Launch:
                if(!prepared)throw std::runtime_error("BG launch requires Prepare before observer starts");
                prepared=false;work_in_flight=true;s.host.bg_submit_begin=ap::monotonic_ns();gpu->launch();s.host.bg_submit_end=ap::monotonic_ns();break;
            case ap::Command::Drain:
                s.host.bg_correct=gpu->correct();s.host.bg_progress_correct=gpu->progress_correct;work_in_flight=false;
                if(group_request_attempted)ap::note_active_result_measured();
                if(rm)rm->mark_work_drained();ap::save_control_journal();break;
            case ap::Command::GroupPreemptWait:
                if(!plan.group_identity||!group)throw std::runtime_error("OBJECT_BINDING_UNAVAILABLE: group command requires BG group identity");
                if(!work_in_flight||!ap::acquire(&s.bg.started))throw std::runtime_error("INVALID_OBJECT_OR_STATE: BG main work not launched/observed");
                s.host.bg_done_at_owner_check=ap::acquire(&s.bg.done);
                if(s.host.bg_done_at_owner_check){s.host.result.rejection=ap::ControlResult::Rejection::TargetCompleted;break;}
                s.host.result=ap::preempt_group_wait(group->identity,ap::group_preempt_timeout_limit_us());
                group_request_attempted=s.host.result.attempted;break; // mmap journal already published
            case ap::Command::CheckReuse:{
                if(work_in_flight)throw std::runtime_error("BG reuse check requires completed drain");
                auto context=gpu->context;auto stream=gpu->stream;
                s.host.bg_reuse_ok=gpu->check_short_reuse();
                std::ofstream f(run_dir+"/bg_context_usability.json");
                f<<std::boolalpha<<"{\"fixed_iterations\":256,\"same_context\":"<<(gpu->context==context)<<",\"same_stream\":"<<(gpu->stream==stream)
                 <<",\"reference_match\":"<<bool(s.host.bg_reuse_ok)<<",\"extra_preempt_requests\":0}\n";break;
            }
            case ap::Command::PreemptWait:need_rm();s.host.result=rm->preempt(true);break;
            case ap::Command::PreemptAsync:need_rm();s.host.result=rm->preempt(false);break;
            case ap::Command::Disable:need_rm();recovery.disabled=true;s.host.result=rm->disable(true,false);break;
            case ap::Command::DisableScheduling:
                need_rm();recovery.disabled=true;s.host.preliminary_result=rm->disable(true,true);
                s.host.result=s.host.preliminary_result.ok()?rm->preempt(true):s.host.preliminary_result;break;
            case ap::Command::Enable:need_rm();s.host.result=rm->disable(false);if(s.host.result.ok())recovery.disabled=false;break;
            case ap::Command::ReadMode:
                if(rm){s.host.result=rm->get_preemption_mode(s.host.bg_mode_after);s.host.bg_mode_status_after=s.host.result.rm_status;identity<<"mode_after_INT_init="<<s.host.bg_mode_after<<" status="<<s.host.result.describe()<<'\n';identity.flush();}break;
            case ap::Command::Exit:running=false;break;
            }
            seen=seq;ap::release(&s.host.ack_seq,seen);
        }
        if(!cleanup())throw std::runtime_error("RECOVERY_FAILED: BG configuration restoration");
        gpu->calibrate_observation(run_dir+"/bg_calibration_after.csv");
        if(!gpu->shutdown())throw std::runtime_error("CUDA_CLEANUP_FAILURE");
        ap::save_rm_observation(run_dir,"bg_after_cleanup_");
        std::ofstream(run_dir+"/bg_status.txt")<<"COMPLETED\n";return 0;
    }catch(const std::exception& e){
        if(!run_dir.empty()){
            std::ofstream(run_dir+"/bg_failure.txt")<<e.what()<<'\n';
            std::ofstream(run_dir+"/bg_capture.txt")<<ap::capture_inventory();
        }
        if(mapping)ap::report_error(*mapping->shared,e.what());
        try{cleanup();}catch(const std::exception& recovery_error){std::cerr<<"RECOVERY_FAILED: "<<recovery_error.what()<<'\n';}
        if(gpu&&gpu->context&&!gpu->error_drain(run_dir+"/bg_error_drain.txt"))std::cerr<<"RECOVERY_UNCONFIRMED: BG work drain; stop subsequent experiments\n";
        std::cerr<<"BG error: "<<e.what()<<'\n';return 1;
    }
}
