#include "worker_common.cuh"
#include "recovery.h"
#include "trial_record.h"
#include "json_log.h"
#include <atomic>
#include <filesystem>
#include <memory>
#include <random>
#include <signal.h>
#include <sys/wait.h>

namespace {
struct Child {
    pid_t pid=-1;ap::Shared& shared;uint32_t seq=0;
    uint64_t send_ns=0,ack_ns=0;std::string run_dir;
    explicit Child(ap::Shared& s,const std::string& dir):shared(s),run_dir(dir){}
    ap::ControlResult command(ap::Command cmd,int64_t trial=-1){
        send_ns=ack_ns=0;
        ap::check_peer(shared);ap::check_stop();shared.host.command=cmd;shared.host.trial_id=trial;
        send_ns=ap::monotonic_ns();ap::release(&shared.host.command_seq,++seq);
        uint64_t deadline=ap::monotonic_ns()+ap::host_timeout_ns;
        while(ap::acquire(&shared.host.ack_seq)!=seq){
            ap::check_stop();ap::check_peer(shared);int status=0;
            pid_t r=waitpid(pid,&status,WNOHANG);if(r==pid){pid=-1;throw std::runtime_error("BG worker exited before command acknowledgment");}
            if(ap::monotonic_ns()>deadline)throw std::runtime_error("CONTROL_TIMEOUT: BG command acknowledgment");ap::relax();
        }
        ack_ns=ap::monotonic_ns();return shared.host.result;
    }
    bool reap(unsigned seconds){
        uint64_t deadline=ap::monotonic_ns()+uint64_t(seconds)*1000000000;
        while(pid>0){int status=0;pid_t r=waitpid(pid,&status,WNOHANG);
            if(r==pid){pid=-1;return WIFEXITED(status)&&WEXITSTATUS(status)==0;}
            if(r<0&&errno==ECHILD){pid=-1;return false;}
            if(ap::monotonic_ns()>deadline)return false;std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }return true;
    }
    void finish(){command(ap::Command::Exit);if(!reap(20))throw std::runtime_error("BG shutdown/recovery did not complete successfully");}
    void stop()noexcept{
        if(pid<=0)return;
        kill(pid,SIGTERM);bool clean=reap(5);
        if(pid>0){kill(pid,SIGKILL);reap(2);}
        try{std::ofstream(run_dir+"/controller_child_cleanup.txt",std::ios::app)
            <<(clean?"BG exited after owner-side cleanup request":"RECOVERY_UNCONFIRMED: BG error/timeout; forced exit may not restore GPU state")<<" remaining_pid="<<pid<<'\n';}catch(...){}
    }
    ~Child(){stop();}
};
struct Observation {uint64_t entry=0,main=0,done=0,max_poll_gap=0;std::vector<std::pair<uint64_t,uint64_t>> heartbeat;};
struct Observer {
    std::atomic<bool> stop{false},ready{false};Observation data;std::thread thread;
    explicit Observer(ap::Shared& s,bool bg){
        data.heartbeat.reserve(ap::max_samples);
        thread=std::thread([this,&s,bg]{
            uint32_t consumed=0;uint64_t previous=ap::monotonic_ns();ready.store(true,std::memory_order_release);
            do{
                uint64_t now=ap::monotonic_ns();data.max_poll_gap=std::max(data.max_poll_gap,now-previous);previous=now;
                if(!data.entry&&ap::acquire(&s.interactive.entry_started))data.entry=ap::monotonic_ns();
                if(!data.main&&ap::acquire(&s.interactive.started))data.main=ap::monotonic_ns();
                if(!data.done&&ap::acquire(&s.interactive.done))data.done=ap::monotonic_ns();
                uint32_t available=bg?std::min(ap::acquire(&s.bg.heartbeat_count),ap::max_samples):0;
                if(available>consumed){uint64_t observed=ap::monotonic_ns();for(;consumed<available;++consumed)data.heartbeat.emplace_back(s.bg.heartbeat_gpu_ns[consumed],observed);}
                ap::relax();
            }while(!stop.load(std::memory_order_acquire));
        });while(!ready.load(std::memory_order_acquire))ap::relax();
    }
    void finish(){stop.store(true,std::memory_order_release);if(thread.joinable())thread.join();}
    ~Observer(){finish();}
};
void snapshot(ap::TrialRecord& r,const ap::Shared& s,const Observer* observer){
    if(observer){r.entry_observed=observer->data.entry;r.main_observed=observer->data.main;r.done_observed=observer->data.done;r.poll_gap=observer->data.max_poll_gap;}
    if(ap::acquire(&s.interactive.entry_started)){r.entry_gpu=s.interactive.entry_gpu_ns;r.entry_node=s.interactive.entry_node;}
    if(ap::acquire(&s.interactive.started)){r.main_gpu=s.interactive.start_gpu_ns;r.main_node=s.interactive.main_node;}
    if(ap::acquire(&s.interactive.done))r.done_gpu=s.interactive.done_gpu_ns;
    if(r.bg_present){if(ap::acquire(&s.bg.started))r.bg_main_gpu=s.bg.start_gpu_ns;if(ap::acquire(&s.bg.done))r.bg_done_gpu=s.bg.done_gpu_ns;r.overflow=s.bg.overflow;}
}
int probe_group_info(const ap::Options& o){
    std::unique_ptr<ap::Telemetry> telemetry;
    std::unique_ptr<ap::GpuWorker> gpu;
    std::string stage="profile";
    auto persist=[&](const std::string& prefix){ap::save_control_journal();ap::save_rm_observation(o.run_dir,prefix);};
    try{
        ap::configure_rm(ap::RmStage::GroupInfo); // never Active; no authorization argument
        ap::open_control_journal(o.run_dir,"probe");
        stage="device_access";
        int fd=open("/dev/nvidiactl",O_RDWR|O_CLOEXEC);
        if(fd<0)throw std::runtime_error("DEVICE_ACCESS_BLOCKED: /dev/nvidiactl errno="+std::to_string(errno));
        close(fd); // access check only, never the control FD
        stage="cuda_scope";
        std::ofstream cuda_calls(o.run_dir+"/probe_cuda_calls.log");
        auto call=[&](CUresult rc,const char* name){cuda_calls<<name<<" code="<<int(rc)<<std::endl;ap::cu_check(rc,name);};
        call(cuInit(0),"cuInit");int count=0;call(cuDeviceGetCount(&count),"cuDeviceGetCount");
        cuda_calls<<"visible_device_count="<<count<<std::endl;
        if(count!=1)throw std::runtime_error("OBJECT_BINDING_UNAVAILABLE: group probe requires one visible CUDA device");
        CUdevice device;CUuuid uuid{};call(cuDeviceGet(&device,0),"cuDeviceGet(0)");call(cuDeviceGetUuid(&uuid,device),"cuDeviceGetUuid");
        cudaUUID_t runtime_uuid{};static_assert(sizeof(uuid)==sizeof(runtime_uuid));std::memcpy(&runtime_uuid,&uuid,sizeof(uuid));
        ap::note_group_cuda_scope(ap::uuid_string(runtime_uuid),count);
        stage="cuda_workload";
        telemetry=std::make_unique<ap::Telemetry>();gpu=std::make_unique<ap::GpuWorker>(*telemetry,300,1,0,false,256,false,true);
        gpu->cleanup_log=o.run_dir+"/probe_cuda_cleanup.log";
        gpu->launch();if(!gpu->correct())throw std::runtime_error("CORRECTNESS_FAILURE: finite current-context workload differs from warmup reference");
        if(ap::uuid_string(gpu->prop.uuid)!=ap::uuid_string(runtime_uuid))throw std::runtime_error("OBJECT_BINDING_UNAVAILABLE: CUDA UUID changed");
        ap::note_cuda_ready(gpu->prop.major,gpu->prop.minor);
        cuda_calls<<"fixed_iterations=256 blocks=1 threads=256 reference_match=true\n";cuda_calls.close();
        std::ofstream cuda_identity(o.run_dir+"/cuda_identity.txt");ap::write_identity(cuda_identity,*gpu);cuda_identity.close();
        stage="group_binding";persist("before_binding_");
        const auto identity=ap::inspect_owned_tsg(); // no GET_INFO in discovery
        std::ofstream candidate(o.run_dir+"/group_identity.json");candidate<<identity.json()<<'\n';candidate.close();
        if(!candidate)throw std::runtime_error("Cannot persist group identity before GET_INFO");
        persist("before_get_info_");
        stage="group_get_info";
        const auto result=ap::get_group_info(identity); // the only project control in this probe
        ap::save_control_journal();
        const auto& r=result.control;
        std::ofstream query(o.run_dir+"/get_info.json");
        query<<std::boolalpha<<"{\"command\":\"NVA06C_CTRL_CMD_GET_INFO\",\"target_scope\":\"group\",\"hClient\":"<<identity.binding().client
             <<",\"hObject\":"<<identity.binding().group.token.handle<<",\"attempted\":"<<r.attempted<<",\"operation_seq\":"<<r.operation_seq
             <<",\"syscall_return\":"<<(r.attempted?std::to_string(r.syscall_result):"null")<<",\"errno\":"<<(r.attempted?std::to_string(r.syscall_errno):"null")
             <<",\"NV_STATUS_raw\":"<<r.rm_status<<",\"NV_STATUS_valid\":"<<(r.attempted&&r.syscall_result==0&&r.rm_status!=0xffffffff)
             <<",\"call_begin_ns\":"<<(r.begin_ns?std::to_string(r.begin_ns):"null")<<",\"call_end_ns\":"<<(r.end_ns?std::to_string(r.end_ns):"null")
             <<",\"get_info_verified\":"<<r.ok()<<",\"result\":"<<ap::json_string(r.describe())
             <<",\"hardware_tsg_id\":"<<(result.hardware_tsg_id?std::to_string(*result.hardware_tsg_id):"null")<<"}\n";
        query.close();if(!query)throw std::runtime_error("Cannot persist GET_INFO output; retain control journal");
        if(!r.ok())throw std::runtime_error("GET_INFO: "+r.describe());
        persist("before_cleanup_");stage="cuda_cleanup";
        bool cleanup=gpu->shutdown();gpu.reset();persist("");
        if(!cleanup)throw std::runtime_error("CUDA_CLEANUP_FAILURE");
        std::ofstream(o.run_dir+"/probe_status.txt")<<"GROUP_GET_INFO_VERIFIED; channel selection unverified; active controls=0; cleanup passed\n";
        return 0;
    }catch(const std::exception& e){
        ap::record_rm_stop_reason(stage+": "+e.what());
        persist("failure_"); // preserve object graph/journal BEFORE cleanup
        std::ofstream(o.run_dir+"/probe_status.txt")<<stage<<": "<<e.what()<<'\n';
        if(gpu){bool cleanup=gpu->shutdown();gpu.reset();std::ofstream(o.run_dir+"/failure_cleanup.txt")<<"cleanup_ok="<<cleanup<<'\n';}
        persist("");std::cerr<<stage<<": "<<e.what()<<'\n';return 77;
    }
}
int probe_rm(const ap::Options& o){
    std::unique_ptr<ap::Telemetry> telemetry;
    std::unique_ptr<ap::GpuWorker> gpu;
    try{
        ap::configure_rm(o.probe==ap::Probe::Observe?ap::RmStage::Observe:ap::RmStage::Readonly);
        int device_fd=open("/dev/nvidiactl",O_RDWR|O_CLOEXEC);
        if(device_fd<0)throw std::runtime_error("DEVICE_ACCESS_BLOCKED: /dev/nvidiactl errno="+std::to_string(errno));
        close(device_fd); // access check only; this FD is NEVER used for RM controls
        if(o.probe!=ap::Probe::Observe)ap::open_control_journal(o.run_dir,"probe");
        telemetry=std::make_unique<ap::Telemetry>();
        gpu=std::make_unique<ap::GpuWorker>(*telemetry,300,1,0,false,256,false,true);
        gpu->cleanup_log=o.run_dir+"/probe_cuda_cleanup.log";
        gpu->launch();if(!gpu->correct())throw std::runtime_error("CORRECTNESS_FAILURE: minimal current-context workload");
        ap::note_cuda_ready(gpu->prop.major,gpu->prop.minor);
        ap::save_rm_observation(o.run_dir,"before_binding_");
        auto binding=ap::inspect_owned_compute_group(); // NO project ioctl
        std::ofstream candidate(o.run_dir+"/candidate.json");
        candidate<<"{\"pid\":"<<getpid()<<",\"client\":"<<binding.client<<",\"fd\":"<<binding.fd
            <<",\"client_generation\":"<<binding.client_generation<<",\"group\":"<<binding.group.handle<<",\"group_generation\":"<<binding.group.generation
            <<",\"compute_channel\":"<<binding.compute_channel.handle<<",\"channel_generation\":"<<binding.compute_channel.generation
            <<",\"fd_source\":\"dup of original allocating nvidiactl open file\",\"authenticated_by_GET_INFO\":false}\n";
        candidate.close();
        std::ofstream identity(o.run_dir+"/identity.txt");
        if(o.probe==ap::Probe::Observe){ap::write_identity(identity,*gpu);}
        else{
            ap::RmControl rm(ap::discover_owned_compute_group());
            ap::write_identity(identity,*gpu,&rm);
            if(o.probe==ap::Probe::Readonly){
                std::ofstream getters(o.run_dir+"/getters.jsonl");
                uint64_t ts=0;auto r=rm.get_timeslice(ts);getters<<"{\"getter\":\"GET_TIMESLICE\",\"result\":"<<ap::json_string(r.describe())<<",\"value\":"<<(r.ok()?std::to_string(ts):"null")<<"}\n";
                uint32_t mode=0xffffffff;r=rm.get_preemption_mode(mode);getters<<"{\"getter\":\"GR_GET_CTXSW_MODES\",\"result\":"<<ap::json_string(r.describe())<<",\"value\":"<<(r.ok()?std::to_string(mode):"null")<<"}\n";
            }
        }
        ap::save_control_journal();ap::save_rm_observation(o.run_dir,"before_cleanup_");
        if(!gpu->shutdown())throw std::runtime_error("CUDA_CLEANUP_FAILURE");
        ap::save_rm_observation(o.run_dir);
        std::ofstream(o.run_dir+"/probe_status.txt")<<(o.probe==ap::Probe::Observe?"CANDIDATE_OBSERVED; project controls=0; readonly/active unverified":"READONLY_VERIFIED_BY_GET_INFO; inspect optional getter statuses; active unverified")<<'\n';return 0;
    }catch(const std::exception& e){
        ap::record_rm_stop_reason(e.what());
        ap::save_rm_observation(o.run_dir);
        std::ofstream(o.run_dir+"/probe_status.txt")<<e.what()<<'\n';ap::save_control_journal();std::cerr<<e.what()<<'\n';return 77;
    }
}
}

int main(int argc,char** argv){
    std::string shared_path,run_dir;
    std::unique_ptr<ap::Mapping> mapping;std::unique_ptr<Child> child;std::unique_ptr<ap::GpuWorker> gpu;std::unique_ptr<ap::RmControl> rm;
    ap::Recovery recovery;bool possibly_disabled=false,evidence_writable=false;
    auto cleanup=[&](){
        ap::save_control_journal();ap::stop_requested=0;
        std::ofstream f(run_dir+"/int_recovery.txt",std::ios::app);bool ok=true;
        if(possibly_disabled&&child&&child->pid>0){try{auto r=child->command(ap::Command::Enable);f<<"BG_ENABLE "<<r.describe()<<'\n';possibly_disabled=!r.ok();ok&=r.ok();}catch(const std::exception& e){f<<"RECOVERY_UNCONFIRMED "<<e.what()<<'\n';ok=false;}}
        if(rm)ok&=recovery.restore(*rm,f);
        f<<(ok?"CONFIGURATION_RESTORED":"RECOVERY_FAILED")<<std::endl;ap::save_control_journal();ap::save_rm_observation(run_dir,"int_");return ok;
    };
    try{
        auto o=ap::parse(argc,argv);run_dir=o.run_dir;auto plan=ap::mode_plan(o.mode);
        if(o.probe==ap::Probe::Cuda){
            auto binary=(std::filesystem::canonical("/proc/self/exe").parent_path()/"preflight_cuda").string();
            char* args[]={binary.data(),nullptr};execv(binary.c_str(),args);throw std::runtime_error("Cannot exec independent CUDA probe, errno="+std::to_string(errno));
        }
        if(run_dir.empty())throw std::invalid_argument("--run-dir required for RM probes and experiments");
        std::filesystem::create_directories(run_dir);
        for(auto name:{"raw.csv","probe_status.txt","int_control_events.bin","probe_control_events.bin","group_identity.json","int_identity.txt","failure.txt"})
            if(std::filesystem::exists(run_dir+"/"+name))throw std::runtime_error("Evidence already exists; use a new run directory");
        evidence_writable=true;
        if(o.probe==ap::Probe::GroupInfo)return probe_group_info(o);
        if(o.probe!=ap::Probe::None)return probe_rm(o);
        ap::configure_rm(plan.identity?ap::RmStage::Active:ap::RmStage::Disabled,o.test_host);
        // Before any fork: verify CUDA availability in a separate probe process
        // through the runner. Workers still check cuInit directly below.
        std::signal(SIGTERM,ap::on_stop);std::signal(SIGINT,ap::on_stop);
        char name[]="/tmp/active-preempt-XXXXXX";int fd=mkstemp(name);if(fd<0)throw std::runtime_error("mkstemp failed");
        if(ftruncate(fd,sizeof(ap::Shared))){close(fd);unlink(name);throw std::runtime_error("shared truncate failed");}close(fd);shared_path=name;
        mapping=std::make_unique<ap::Mapping>(shared_path.c_str());auto& s=*mapping->shared;new(&s)ap::Shared{};s.host.controller_pid=getpid();
        if(plan.background){
            child=std::make_unique<Child>(s,run_dir);
            auto bg=(std::filesystem::canonical("/proc/self/exe").parent_path()/"bg_worker").string();
            std::vector<std::string> args={bg,"--shared",shared_path};for(int i=1;i<argc;++i)args.push_back(argv[i]);
            std::vector<char*> ptrs;for(auto& a:args)ptrs.push_back(a.data());ptrs.push_back(nullptr);
            child->pid=fork();if(child->pid<0)throw std::runtime_error("fork failed");if(child->pid==0){execv(bg.c_str(),ptrs.data());_exit(127);}
            uint64_t deadline=ap::monotonic_ns()+ap::host_timeout_ns;
            while(!ap::acquire(&s.host.ready)){ap::check_stop();ap::check_peer(s);if(ap::monotonic_ns()>deadline)throw std::runtime_error("BG initialization timeout");std::this_thread::yield();}
        }
        ap::open_control_journal(run_dir,"int");
        gpu=std::make_unique<ap::GpuWorker>(s.interactive,o.int_us,1,o.heartbeat_ns,o.graph,o.int_iterations,o.diagnostic);
        gpu->cleanup_log=run_dir+"/int_cuda_cleanup.log";
        ap::note_cuda_ready(gpu->prop.major,gpu->prop.minor);
        gpu->calibrate_observation(run_dir+"/int_calibration_before.csv");
        std::ofstream(run_dir+"/int_capture.txt")<<ap::capture_inventory();
        if(plan.identity)rm=std::make_unique<ap::RmControl>(ap::discover_owned_compute_group());
        if(child&&(s.host.bg_pid==uint32_t(getpid())||ap::uuid_string(gpu->prop.uuid)!=s.host.bg_uuid))throw std::runtime_error("Need distinct processes/contexts and same GPU UUID");
        if(plan.identity&&(!s.host.bg_identity_valid||s.host.bg_tsg==rm->identity().tsg_id))throw std::runtime_error("Need verified distinct hardware TSG IDs");
        std::ofstream identity(run_dir+"/int_identity.txt");ap::write_identity(identity,*gpu,rm.get());
        identity<<"mode="<<o.mode<<" force="<<o.force<<" bypass="<<o.bypass<<" graph="<<o.graph<<" run_kind="<<(o.diagnostic?"diagnostic":"performance")<<" trials="<<o.trials<<"\n";
        bool realtime_accepted=false;
        if(plan.timeslice){
            ap::require_ok(rm->get_timeslice(recovery.original_timeslice),"INT GET_TIMESLICE");recovery.timeslice=true;ap::require_ok(rm->timeslice(1000000),"INT SET_TIMESLICE");
            uint64_t readback=0;auto r=rm->get_timeslice(readback);identity<<"timeslice_requested_us=1000000 readback_us="<<(r.ok()?std::to_string(readback):"unknown")<<" get_status="<<r.describe()<<" hardware_quantum_us=unknown\n";
        }
        if(plan.realtime){recovery.realtime=true;ap::require_ok(rm->realtime(true),"MAKE_REALTIME(INT)");realtime_accepted=true;}
        if(child&&plan.identity)child->command(ap::Command::ReadMode);
        identity.flush();ap::save_control_journal();
        std::ofstream configuration(run_dir+"/configuration.json");
        configuration<<"{\"schema_version\":2,\"mode\":"<<ap::json_string(o.mode)
            <<",\"run_kind\":"<<ap::json_string(o.diagnostic?"diagnostic":"performance")
            <<",\"gpu_uuid\":"<<ap::json_string(ap::uuid_string(gpu->prop.uuid))
            <<",\"driver_profile\":"<<ap::json_string(plan.identity?ap::build_profile().version:"CUDA_BASELINE_NO_RM_PROFILE")
            <<",\"graph\":"<<o.graph<<",\"force\":"<<o.force<<",\"bypass\":"<<o.bypass
            <<",\"cta_waves\":"<<o.waves<<",\"heartbeat_ns\":"<<o.heartbeat_ns<<",\"timeslice_us\":"<<o.timeslice_us
            <<",\"bg_target_us\":"<<o.bg_us<<",\"int_target_us\":"<<o.int_us
            <<",\"bg_iterations\":"<<(child?std::to_string(s.host.bg_iterations):"null")
            <<",\"int_iterations\":"<<gpu->iterations<<",\"bg_blocks\":"<<(child?std::to_string(s.host.bg_blocks):"null")
            <<",\"int_blocks\":"<<gpu->blocks<<",\"threads_per_block\":256,\"runlist_policy\":\"unknown\"}\n";
        configuration.flush();if(!configuration)throw std::runtime_error("Cannot save run configuration");
        std::ofstream raw(run_dir+"/raw.csv"),heartbeats,ctas;
        if(child){heartbeats.open(run_dir+"/heartbeats.csv");ctas.open(run_dir+"/bg_ctas.csv");}
        if(!raw||(child&&(!heartbeats||!ctas)))throw std::runtime_error("Cannot open output files");
        ap::TrialRecord::header(raw);if(child){heartbeats<<"schema_version,trial,sample,gpu_ns,cpu_observed_ns\n";ctas<<"schema_version,trial,cta,start_gpu_ns,end_gpu_ns\n";}
        std::minstd_rand rng(20260917);
        for(unsigned trial=0;trial<o.trials;++trial){
            ap::TrialRecord row;row.trial=trial;row.mode=o.mode;row.graph=o.graph;row.force=o.force;row.bypass=o.bypass;row.bg_present=plan.background;row.run_kind=o.diagnostic?"diagnostic":"performance";
            std::unique_ptr<Observer> observer;
            try{
                ap::record_trial(trial);gpu->reset();gpu->host->launch_id=trial;
                if(child){child->command(ap::Command::Launch,trial);ap::wait_value(&s.bg.started,1,"BG main_entry");row.bg_main_observed=ap::monotonic_ns();
                    uint64_t trigger_at=ap::monotonic_ns()+(1000+rng()%4000)*1000ull;
                    while(ap::monotonic_ns()<trigger_at){ap::check_stop();ap::check_peer(s);ap::relax();}
                }
                observer=std::make_unique<Observer>(s,plan.background);
                if(child)row.bg_done_before=ap::acquire(&s.bg.done)!=0;
                row.trigger=ap::monotonic_ns();row.submit_begin=ap::monotonic_ns();gpu->launch();row.submit_end=ap::monotonic_ns();
                auto trigger=plan.trigger(bool(rm),realtime_accepted);
                if(child){
                    ap::Command cmd=ap::Command::None;
                    if(trigger==ap::Trigger::PreemptWait)cmd=ap::Command::PreemptWait;
                    else if(trigger==ap::Trigger::PreemptAsync)cmd=ap::Command::PreemptAsync;
                    else if(trigger==ap::Trigger::Disable){possibly_disabled=true;cmd=ap::Command::Disable;}
                    else if(trigger==ap::Trigger::DisableSplit){possibly_disabled=true;cmd=ap::Command::DisableScheduling;}
                    row.bg_done_before_control=ap::acquire(&s.bg.done)!=0;
                    row.control_pending=cmd!=ap::Command::None;
                    ap::ControlResult r;
                    try{r=child->command(cmd,trial);}catch(...){row.ipc_send=child->send_ns;throw;}
                    row.ipc_send=child->send_ns;row.ipc_received=s.host.command_received_ns;row.ipc_ack=child->ack_ns;
                    if(cmd!=ap::Command::None)row.control=r;
                    row.control_pending=false;
                }
                if(trigger==ap::Trigger::Restart){row.control_pending=true;row.control=rm->restart(o.force,o.bypass);row.control_pending=false;}
                if(trigger!=ap::Trigger::None&&!row.control.ok())throw std::runtime_error(std::string("Trigger rejected: ")+row.control.describe());
                ap::wait_value(&s.interactive.done,1,"INT graph_done");
                if(possibly_disabled){auto r=child->command(ap::Command::Enable,trial);possibly_disabled=!r.ok();ap::require_ok(r,"BG ENABLE");}
                row.int_correct=gpu->correct();if(o.diagnostic)row.progress_correct=gpu->progress_correct;
                if(rm)rm->mark_work_drained();
                if(child){ap::wait_value(&s.bg.done,1,"BG completion");child->command(ap::Command::Drain,trial);row.bg_correct=s.host.bg_correct;if(o.diagnostic)row.progress_correct&=s.host.bg_progress_correct;}
                observer->finish();snapshot(row,s,observer.get());row.complete=true;row.write(raw);
                if(row.control.attempted)ap::note_active_result_measured();
                for(size_t i=0;i<observer->data.heartbeat.size();++i)heartbeats<<2<<','<<trial<<','<<i<<','<<observer->data.heartbeat[i].first<<','<<observer->data.heartbeat[i].second<<'\n';
                if(child)for(unsigned b=0;b<s.host.bg_blocks;++b)ctas<<2<<','<<trial<<','<<b<<','<<s.bg.cta_start_gpu_ns[b]<<','<<s.bg.cta_end_gpu_ns[b]<<'\n';
                ap::save_control_journal();
                if(row.int_correct!=1||(child&&row.bg_correct!=1))throw std::runtime_error("CORRECTNESS_FAILURE: output/progress differs from reference");
            }catch(const std::exception& e){
                if(observer)observer->finish();snapshot(row,s,observer.get());row.failure=e.what();
                if(!row.complete)row.write(raw); // known facts persist even without full trial
                ap::save_control_journal();throw;
            }
        }
        if(!cleanup())throw std::runtime_error("RECOVERY_FAILED: INT/BG configuration");
        gpu->calibrate_observation(run_dir+"/int_calibration_after.csv");if(child)child->finish();
        if(!gpu->shutdown())throw std::runtime_error("CUDA_CLEANUP_FAILURE");
        std::ofstream(run_dir+"/status.txt")<<"COMPLETED: inspect schema 2 dimensions; CONTROL_ACCEPTED is not PREEMPTION_CONFIRMED\n";
        unlink(shared_path.c_str());shared_path.clear();return 0;
    }catch(const std::exception& e){
        std::cerr<<"Error: "<<e.what()<<'\n';
        if(evidence_writable&&!run_dir.empty()&&std::filesystem::is_directory(run_dir)){
            // Before cleanup or context destructors: save the first failure and all
            // returned controls. mmap journals remain recoverable after SIGKILL.
            ap::record_rm_stop_reason(e.what());std::ofstream(run_dir+"/failure.txt")<<e.what()<<'\n';std::ofstream(run_dir+"/int_capture.txt")<<ap::capture_inventory();
            try{cleanup();}catch(const std::exception& r){std::ofstream(run_dir+"/recovery_failure.txt")<<r.what()<<'\n';}
        }
        if(child)child->stop();if(!shared_path.empty())unlink(shared_path.c_str());return 1;
    }
}
