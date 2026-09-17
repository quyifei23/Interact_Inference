#include "worker_common.cuh"
#include <memory>
#include <csignal>
namespace { volatile std::sig_atomic_t stopping=0; void on_stop(int){stopping=1;} }
int main(int argc,char** argv){
    std::unique_ptr<ap::Mapping> mapping;
    try {
        auto o=ap::parse(argc,argv);
        std::signal(SIGTERM,on_stop);std::signal(SIGINT,on_stop);
        if(o.shared_path.empty())throw std::invalid_argument("bg_worker is launched by int_worker; --shared required");
        mapping=std::make_unique<ap::Mapping>(o.shared_path.c_str());auto& s=*mapping->shared;
        ap::GpuWorker gpu(s.bg,o.bg_us,o.waves,o.heartbeat_ns,o.graph);
        gpu.calibrate_observation(o.run_dir+"/bg_calibration_before.csv");
        std::ofstream inventory(o.run_dir+"/bg_capture.txt");inventory<<ap::capture_inventory();inventory.close();
        ap::RmControl rm(ap::discover_owned_compute_group());
        std::ofstream identity(o.run_dir+"/bg_identity.txt");ap::write_identity(identity,gpu,rm);
        uint64_t original_ts=0;bool changed_ts=false,disabled=false;
        if(o.mode=="timeslice"){
            ap::require_ok(rm.get_timeslice(original_ts),"BG get timeslice");
            ap::require_ok(rm.timeslice(o.timeslice_us),"BG set timeslice");changed_ts=true;
            uint64_t cached=0;auto result=rm.get_timeslice(cached);
            identity<<"timeslice_requested_us="<<o.timeslice_us<<" cached_us="<<cached<<" get_status="<<result.describe()<<" hardware_quantum_us=unknown\n";
        }
        auto mode_result=rm.get_preemption_mode(s.host.bg_mode_before);s.host.bg_mode_status_before=mode_result.rm_status;
        identity<<"mode_before="<<s.host.bg_mode_before<<" status="<<mode_result.describe()<<"\n";identity.flush();
        s.host.bg_pid=getpid();s.host.bg_context=reinterpret_cast<uintptr_t>(gpu.context);
        s.host.bg_tsg=rm.identity().tsg_id;s.host.bg_client=rm.identity().client;s.host.bg_group=rm.identity().group;s.host.bg_engine=rm.identity().engine;
        s.host.bg_iterations=gpu.iterations;s.host.bg_solo_us=gpu.solo_us;s.host.bg_uninstrumented_us=gpu.uninstrumented_us;
        std::strncpy(s.host.bg_uuid,ap::uuid_string(gpu.prop.uuid).c_str(),sizeof(s.host.bg_uuid)-1);
        ap::release(&s.host.ready,1);
        uint32_t seen=0;bool running=true;
        try {
            while(running){
                uint64_t deadline=ap::monotonic_ns()+60ull*1000000000;
                while(ap::acquire(&s.host.command_seq)==seen && !stopping){if(ap::monotonic_ns()>deadline)throw std::runtime_error("Coordinator disappeared");ap::relax();}
                if(stopping)break;
                uint32_t seq=ap::acquire(&s.host.command_seq);s.host.result={};s.host.preliminary_result={};
                switch(s.host.command){
                case ap::Command::Launch:
                    gpu.reset();s.host.bg_correct=0;s.host.bg_submit_begin=ap::monotonic_ns();gpu.launch();s.host.bg_submit_end=ap::monotonic_ns();break;
                case ap::Command::Drain:
                    s.host.bg_correct=gpu.correct();break;
                case ap::Command::PreemptWait:s.host.result=rm.preempt(true);break;
                case ap::Command::PreemptAsync:s.host.result=rm.preempt(false);break;
                case ap::Command::Disable:
                    disabled=true;s.host.result=rm.disable(true,false);break;
                case ap::Command::DisableScheduling:
                    disabled=true;s.host.preliminary_result=rm.disable(true,true);
                    if(s.host.preliminary_result.ok())s.host.result=rm.preempt(true);else s.host.result=s.host.preliminary_result;break;
                case ap::Command::Enable:
                    s.host.result=rm.disable(false);if(s.host.result.ok())disabled=false;break;
                case ap::Command::ReadMode:
                    s.host.result=rm.get_preemption_mode(s.host.bg_mode_after);s.host.bg_mode_status_after=s.host.result.rm_status;
                    identity<<"mode_after_INT_init="<<s.host.bg_mode_after<<" status="<<s.host.result.describe()<<"\n";identity.flush();break;
                case ap::Command::Exit:running=false;break;
                default:throw std::runtime_error("Unexpected command");
                }
                seen=seq;ap::release(&s.host.ack_seq,seen);
            }
        }catch(...){if(disabled){auto r=rm.disable(false);std::cerr<<"Emergency owner-side enable: "<<r.describe()<<'\n';}if(changed_ts)rm.timeslice(original_ts);throw;}
        if(disabled)ap::require_ok(rm.disable(false),"final BG enable");
        if(changed_ts)ap::require_ok(rm.timeslice(original_ts),"restore BG timeslice");
        gpu.calibrate_observation(o.run_dir+"/bg_calibration_after.csv");
        return 0;
    }catch(const std::exception& e){if(mapping)ap::report_error(*mapping->shared,e.what());std::cerr<<"BG error: "<<e.what()<<'\n';return 1;}
}
