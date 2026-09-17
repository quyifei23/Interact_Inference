#include "worker_common.cuh"
#include <atomic>
#include <filesystem>
#include <memory>
#include <random>
#include <signal.h>
#include <sys/wait.h>

namespace {
struct Child {
    pid_t pid=-1;ap::Shared& shared;uint32_t seq=0;
    explicit Child(ap::Shared& s):shared(s){}
    ap::ControlResult command(ap::Command c){
        ap::check_peer(shared);shared.host.command=c;ap::release(&shared.host.command_seq,++seq);
        uint64_t deadline=ap::monotonic_ns()+ap::host_timeout_ns;
        while(ap::acquire(&shared.host.ack_seq)!=seq){ap::check_peer(shared);if(ap::monotonic_ns()>deadline)throw std::runtime_error("BG command timeout");ap::relax();}
        return shared.host.result;
    }
    void finish(){
        command(ap::Command::Exit);
        uint64_t deadline=ap::monotonic_ns()+ap::host_timeout_ns;int status=0;
        while(waitpid(pid,&status,WNOHANG)==0){if(ap::monotonic_ns()>deadline)throw std::runtime_error("BG shutdown timeout");std::this_thread::yield();}
        pid=-1;if(!WIFEXITED(status)||WEXITSTATUS(status)!=0)throw std::runtime_error("BG exited with failure");
    }
    ~Child(){if(pid>0){kill(pid,SIGTERM);waitpid(pid,nullptr,0);}}
};
struct Observation {
    uint64_t start=0,done=0,max_poll_gap=0;
    std::vector<std::pair<uint64_t,uint64_t>> heartbeat; // GPU ns, CPU observed ns
};
struct Observer {
    std::atomic<bool> stop{false},ready{false};Observation data;std::thread thread;
    explicit Observer(ap::Shared& s){
        data.heartbeat.reserve(ap::max_samples);
        thread=std::thread([this,&s]{
            uint32_t consumed=0;uint64_t previous=ap::monotonic_ns();ready.store(true,std::memory_order_release);
            while(!stop.load(std::memory_order_acquire)){
                uint64_t now=ap::monotonic_ns();data.max_poll_gap=std::max(data.max_poll_gap,now-previous);previous=now;
                if(!data.start && ap::acquire(&s.interactive.started))data.start=ap::monotonic_ns();
                if(!data.done && ap::acquire(&s.interactive.done))data.done=ap::monotonic_ns();
                uint32_t available=std::min(ap::acquire(&s.bg.heartbeat_count),ap::max_samples);
                if(available>consumed){uint64_t observed=ap::monotonic_ns();
                    // Backlogged samples share observation time. This is retained
                    // explicitly; it cannot be used as an exact preempt timestamp.
                    for(;consumed<available;++consumed)data.heartbeat.emplace_back(s.bg.heartbeat_gpu_ns[consumed],observed);
                }
                ap::relax();
            }
        });
        while(!ready.load(std::memory_order_acquire))ap::relax();
    }
    void finish(){stop.store(true,std::memory_order_release);if(thread.joinable())thread.join();}
    ~Observer(){finish();}
};
std::string optional(uint64_t n){return n?std::to_string(n):"";}
void save_telemetry(std::ostream& hb,std::ostream& cta,unsigned trial,const ap::Shared& s,const Observation& obs,unsigned bg_blocks){
    for(size_t i=0;i<obs.heartbeat.size();++i)hb<<trial<<','<<i<<','<<obs.heartbeat[i].first<<','<<obs.heartbeat[i].second<<'\n';
    for(unsigned b=0;b<bg_blocks;++b)cta<<trial<<','<<b<<','<<s.bg.cta_start_gpu_ns[b]<<','<<s.bg.cta_end_gpu_ns[b]<<'\n';
}
}

int main(int argc,char** argv){
    std::string path,run_dir;
    try {
        auto o=ap::parse(argc,argv);run_dir=o.run_dir;
        if(access("/dev/nvidiactl",R_OK|W_OK)!=0){std::cerr<<"SKIP: no accessible /dev/nvidiactl; no GPU benchmark results produced\n";return 77;}
        if(!ap::baseline_driver_loaded()){std::cerr<<"SKIP: prototype is pinned to NVIDIA 550.120; loaded version differs\n";return 77;}
        if(o.probe){CU_OK(cuInit(0));int count=0;CU_OK(cuDeviceGetCount(&count));std::cout<<"CUDA devices="<<count<<'\n';return count?0:77;}
        if(run_dir.empty())throw std::invalid_argument("--run-dir is required");
        std::filesystem::create_directories(run_dir);
        if(std::filesystem::exists(run_dir+"/raw.csv")||std::filesystem::exists(run_dir+"/int_identity.txt"))throw std::runtime_error("Run directory already contains evidence; refusing overwrite");
        char shared_path[]="/tmp/active-preempt-XXXXXX";int fd=mkstemp(shared_path);
        if(fd<0)throw std::runtime_error("mkstemp failed");
        if(ftruncate(fd,sizeof(ap::Shared))!=0){close(fd);unlink(shared_path);throw std::runtime_error("shared truncate failed");}close(fd);path=shared_path;
        ap::Mapping mapping(path.c_str());auto& s=*mapping.shared;new(&s)ap::Shared{};
        Child child(s);
        // fork+exec BEFORE creating any CUDA context or initializing CUDA in parent.
        std::filesystem::path executable=std::filesystem::canonical("/proc/self/exe");
        std::string bg=(executable.parent_path()/"bg_worker").string();
        std::vector<std::string> args={bg,"--shared",path};for(int i=1;i<argc;++i)args.push_back(argv[i]);
        std::vector<char*> bg_args;for(auto& a:args)bg_args.push_back(a.data());bg_args.push_back(nullptr);
        child.pid=fork();if(child.pid<0)throw std::runtime_error("fork failed");
        if(child.pid==0){execv(bg.c_str(),bg_args.data());_exit(127);}
        uint64_t deadline=ap::monotonic_ns()+ap::host_timeout_ns;
        while(!ap::acquire(&s.host.ready)){ap::check_peer(s);if(ap::monotonic_ns()>deadline)throw std::runtime_error("BG initialization timeout");std::this_thread::yield();}
        ap::GpuWorker gpu(s.interactive,o.int_us,1,o.heartbeat_ns,o.graph);
        gpu.calibrate_observation(run_dir+"/int_calibration_before.csv");
        {std::ofstream inventory(run_dir+"/int_capture.txt");inventory<<ap::capture_inventory();}
        ap::RmControl rm(ap::discover_owned_compute_group());
        if(s.host.bg_pid==uint32_t(getpid()) || s.host.bg_tsg==rm.identity().tsg_id ||
           ap::uuid_string(gpu.prop.uuid)!=s.host.bg_uuid)
            throw std::runtime_error("Need separate processes/contexts, distinct hardware TSG IDs, and the same GPU UUID");
        std::ofstream identity(run_dir+"/int_identity.txt");ap::write_identity(identity,gpu,rm);
        identity<<"context_distinct_evidence=explicit_cuCtxCreate_in_distinct_processes; pointer_values_alone_are_not_identity\n";
        identity<<"mode="<<o.mode<<" force="<<o.force<<" bypass="<<o.bypass<<" graph="<<o.graph<<" trials="<<o.trials<<"\n";
        bool rt=false,ts=false,possibly_disabled=false;uint64_t original_ts=0;
        uint32_t int_mode=0xffffffff;auto mode_result=rm.get_preemption_mode(int_mode);
        identity<<"compute_mode_before="<<int_mode<<" status="<<mode_result.describe()<<'\n';
        try {
            if(o.mode=="timeslice"){
                ap::require_ok(rm.get_timeslice(original_ts),"INT get timeslice");ap::require_ok(rm.timeslice(1000000),"INT set timeslice");ts=true;
            }
            if(o.mode=="realtime"){
                ap::require_ok(rm.realtime(true),"MAKE_REALTIME(INT), requires NICE authorization");rt=true;
                mode_result=rm.get_preemption_mode(int_mode);
                identity<<"compute_mode_after="<<int_mode<<" status="<<mode_result.describe()<<'\n';
            }
            child.command(ap::Command::ReadMode);
            identity<<"bg_mode_before="<<s.host.bg_mode_before<<" bg_mode_after="<<s.host.bg_mode_after<<" bg_mode_status_after="<<s.host.bg_mode_status_after<<'\n';identity.flush();
            std::ofstream raw(run_dir+"/raw.csv"),heartbeats(run_dir+"/heartbeats.csv"),ctas(run_dir+"/bg_ctas.csv");
            if(!raw||!heartbeats||!ctas)throw std::runtime_error("Cannot write result files");
            raw<<"trial,mode,force,bypass,graph,T_cpu_trigger,T_int_submit_end,T_rm_call_begin,T_rm_call_end,rm_syscall_result,rm_errno,rm_status,T_int_gpu_start_observed,T_int_gpu_done_observed,T_bg_preempted_observed,T_bg_resumed_observed,T_int_gpu_start_ns,T_int_gpu_done_ns,T_bg_done_gpu_ns,T_bg_gap_begin_gpu_proxy,T_bg_gap_end_gpu_proxy,T_bg_activity_after_int_observed,T_reenable_begin,T_reenable_end,preliminary_rm_begin,preliminary_rm_end,observer_max_poll_gap_ns,bg_correct,int_correct,heartbeat_overflow,classification\n";
            heartbeats<<"trial,sample,gpu_ns,cpu_observed_ns\n";ctas<<"trial,cta,start_gpu_ns,end_gpu_ns\n";
            std::minstd_rand rng(20260917);
            for(unsigned trial=0;trial<o.trials;++trial){
                gpu.reset();child.command(ap::Command::Launch);
                ap::wait_value(&s.bg.started,1,"BG main kernel start");
                // Randomize arrival phase, far inside the 50-100ms arithmetic kernel.
                uint64_t trigger_at=ap::monotonic_ns()+(1000+rng()%4000)*1000ull;
                while(ap::monotonic_ns()<trigger_at){ap::check_peer(s);ap::relax();}
                if(ap::acquire(&s.bg.done))throw std::runtime_error("BG finished before interaction; trial setup invalid");
                Observer observer(s); // independent of blocking RM calls
                uint64_t trigger=ap::monotonic_ns();gpu.launch();uint64_t submit_end=ap::monotonic_ns();
                ap::ControlResult result,preliminary,enable;
                if(o.mode=="preempt-wait")result=child.command(ap::Command::PreemptWait);
                else if(o.mode=="preempt-async")result=child.command(ap::Command::PreemptAsync);
                else if(o.mode=="realtime")result=rm.restart(o.force,o.bypass);
                else if(o.mode=="disable"||o.mode=="disable-split"){
                    possibly_disabled=true;result=child.command(o.mode=="disable"?ap::Command::Disable:ap::Command::DisableScheduling);preliminary=s.host.preliminary_result;
                }
                bool control_ok=!result.begin_ns || result.ok();
                // Restore immediately on error; keep syscall and RM failure in raw data.
                if(!control_ok && possibly_disabled){enable=child.command(ap::Command::Enable);possibly_disabled=!enable.ok();}
                ap::wait_value(&s.interactive.done,1,"INT completion marker");
                if(possibly_disabled){enable=child.command(ap::Command::Enable);possibly_disabled=!enable.ok();ap::require_ok(enable,"BG re-enable");}
                ap::wait_value(&s.bg.done,1,"BG completion marker");
                bool int_correct=gpu.correct();child.command(ap::Command::Drain);
                // Allow observer to record both publication markers before joining.
                uint64_t settle=ap::monotonic_ns()+1000000;while(ap::monotonic_ns()<settle)ap::relax();observer.finish();
                const auto& observed=observer.data;
                uint64_t gap_begin=0,gap_end=0,resumed_activity=0;
                for(size_t k=0;k<observed.heartbeat.size();++k){
                    auto [g,c]=observed.heartbeat[k];
                    if(!resumed_activity && g>s.interactive.done_gpu_ns)resumed_activity=c;
                    if(k && observed.heartbeat[k-1].first<s.interactive.start_gpu_ns && g>s.interactive.start_gpu_ns){gap_begin=observed.heartbeat[k-1].first;gap_end=g;}
                }
                std::string classification="observed";
                if(!control_ok)classification="rm_error";
                else if(!observed.start||!observed.done||observed.start<trigger)classification="invalid_observation";
                else if(result.begin_ns && observed.start<result.begin_ns)classification="int_before_rm_issue";
                else if(s.interactive.start_gpu_ns>=s.bg.done_gpu_ns)classification="int_after_bg_done";
                if(!int_correct || !s.host.bg_correct)classification="state_mismatch";
                raw<<trial<<','<<o.mode<<','<<o.force<<','<<o.bypass<<','<<o.graph<<','<<trigger<<','<<submit_end<<','<<optional(result.begin_ns)<<','<<optional(result.end_ns)<<',';
                if(result.begin_ns)raw<<result.syscall_result<<','<<result.syscall_errno<<','<<result.rm_status;else raw<<",,";
                raw<<','<<optional(observed.start)<<','<<optional(observed.done)<<",,," // no fabricated exact BG preempt/resume timestamps
                   <<s.interactive.start_gpu_ns<<','<<s.interactive.done_gpu_ns<<','<<s.bg.done_gpu_ns<<','<<optional(gap_begin)<<','<<optional(gap_end)<<','<<optional(resumed_activity)<<','
                   <<optional(enable.begin_ns)<<','<<optional(enable.end_ns)<<','<<optional(preliminary.begin_ns)<<','<<optional(preliminary.end_ns)<<','<<observed.max_poll_gap<<','<<s.host.bg_correct<<','<<int_correct<<','<<s.bg.overflow<<','<<classification<<'\n';
                raw.flush();save_telemetry(heartbeats,ctas,trial,s,observed,2*gpu.prop.multiProcessorCount*o.waves);
                if(!control_ok)throw std::runtime_error("RM control failed; recorded in raw.csv: "+result.describe());
                if(!int_correct || !s.host.bg_correct)throw std::runtime_error("BG/INT output differs from solo reference");
                if(trial%100==0)std::cout<<"trial "<<trial<<'/'<<o.trials<<" classification="<<classification<<std::endl;
            }
            if(rt){ap::require_ok(rm.realtime(false),"demote INT realtime");rt=false;}
            if(ts){ap::require_ok(rm.timeslice(original_ts),"restore INT timeslice");ts=false;}
            gpu.calibrate_observation(run_dir+"/int_calibration_after.csv");
            child.finish();
        }catch(...){
            if(possibly_disabled){try{auto r=child.command(ap::Command::Enable);std::cerr<<"Cleanup BG enable: "<<r.describe()<<'\n';}catch(const std::exception& e){std::cerr<<"BG recovery error: "<<e.what()<<'\n';}}
            if(rt)std::cerr<<"Cleanup RT demotion: "<<rm.realtime(false).describe()<<'\n';
            if(ts)std::cerr<<"Cleanup timeslice: "<<rm.timeslice(original_ts).describe()<<'\n';
            throw;
        }
        unlink(path.c_str());path.clear();
        std::ofstream(run_dir+"/status.txt")<<"COMPLETED: measured trials; inspect classifications, calibration and raw data before drawing conclusions\n";
        return 0;
    }catch(const std::exception& e){
        if(!path.empty())unlink(path.c_str());
        if(!run_dir.empty() && std::filesystem::is_directory(run_dir))std::ofstream(run_dir+"/failure.txt")<<e.what()<<'\n';
        std::cerr<<"Error: "<<e.what()<<'\n';return 1;
    }
}
