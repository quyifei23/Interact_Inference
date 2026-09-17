#pragma once
#include "rm_control.h"
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <csignal>
#include <type_traits>

namespace ap {
constexpr uint32_t max_samples=65536, max_blocks=4096;
constexpr uint64_t gpu_watchdog_ns=2000000000ull, host_timeout_ns=15000000000ull;
inline volatile std::sig_atomic_t stop_requested=0;
inline void on_stop(int){stop_requested=1;}
inline void check_stop(){if(stop_requested)throw std::runtime_error("STOP_REQUESTED: owner cleanup required");}
inline uint32_t acquire(const uint32_t* p){return __atomic_load_n(p,__ATOMIC_ACQUIRE);}
inline void release(uint32_t* p,uint32_t v){__atomic_store_n(p,v,__ATOMIC_RELEASE);}
inline void relax(){asm volatile("pause" ::: "memory");}
inline void wait_value(const uint32_t* p,uint32_t value,const char* what) {
    uint64_t deadline=monotonic_ns()+host_timeout_ns;
    while(acquire(p)!=value){check_stop();if(monotonic_ns()>deadline)throw std::runtime_error(std::string("OBSERVATION_TIMEOUT: ")+what);relax();}
}
struct alignas(4096) Telemetry {
    alignas(64) uint32_t entry_started=0;
    alignas(64) uint32_t started=0;
    alignas(64) uint32_t done=0;
    alignas(64) uint32_t ping_request=0;
    alignas(64) uint32_t ping_ack=0;
    alignas(64) uint32_t heartbeat_count=0;
    uint32_t overflow=0;
    uint64_t entry_gpu_ns=0,start_gpu_ns=0, done_gpu_ns=0, ping_gpu_ns=0;
    uint32_t entry_node=0,main_node=0;
    uint64_t launch_id=0;
    uint64_t heartbeat_gpu_ns[max_samples]{};
    uint64_t cta_start_gpu_ns[max_blocks]{}, cta_end_gpu_ns[max_blocks]{};
};
enum class Command:uint32_t { None, Launch, Drain, PreemptWait, PreemptAsync, Disable, DisableScheduling, Enable, ReadMode, Exit, Prepare, GroupPreemptWait, CheckReuse };
struct alignas(4096) HostState {
    uint32_t ready=0,error=0,command_seq=0,ack_seq=0,controller_pid=0;
    Command command=Command::None;
    uint32_t bg_correct=0,bg_pid=0,bg_tsg=0xffffffff,bg_client=0,bg_group=0,bg_engine=0;
    uint32_t bg_identity_valid=0,bg_blocks=0,bg_progress_correct=0;
    uint32_t bg_group_identity_valid=0,bg_done_at_owner_check=2,bg_reuse_ok=0;
    uint32_t bg_mode_before=0xffffffff,bg_mode_after=0xffffffff;
    uint32_t bg_mode_status_before=0xffffffff,bg_mode_status_after=0xffffffff;
    uint64_t bg_context=0,bg_iterations=0,bg_submit_begin=0,bg_submit_end=0;
    int64_t trial_id=-1;
    uint64_t command_received_ns=0;
    double bg_solo_us=0,bg_uninstrumented_us=0;
    ControlResult result{},preliminary_result{};
    char error_message[512]{},bg_uuid[64]{};
};
// TSG IDs are compared only for the same selected GPU and explicit matching
// engine scope. Runlist ID remains unmeasured; RM handle numbers are irrelevant.
inline bool distinct_group_scope(uint32_t bg_pid,uint32_t int_pid,const std::string& bg_uuid,const std::string& int_uuid,
                                 uint32_t bg_engine,uint32_t int_engine,uint32_t bg_tsg,uint32_t int_tsg){
    return bg_pid&&int_pid&&bg_pid!=int_pid&&!bg_uuid.empty()&&bg_uuid==int_uuid&&bg_engine&&bg_engine==int_engine&&bg_tsg!=int_tsg;
}
struct Shared { HostState host; Telemetry bg; Telemetry interactive; };
static_assert(sizeof(Telemetry)%4096==0 && sizeof(HostState)%4096==0);
static_assert(std::is_trivially_copyable<ControlResult>::value);
static_assert(offsetof(Telemetry,entry_started)%4==0&&offsetof(Telemetry,entry_gpu_ns)%8==0);
static_assert(offsetof(Telemetry,heartbeat_gpu_ns)%8==0);
static_assert(__atomic_always_lock_free(4,nullptr),"CPU/GPU mailbox requires lock-free natural 32-bit loads/stores");
struct Mapping {
    int fd=-1;Shared* shared=nullptr;
    explicit Mapping(const char* path,bool create=false){
        fd=open(path,O_RDWR|(create?O_CREAT|O_EXCL:0),0600);
        if(fd<0)throw std::runtime_error("Cannot open shared mapping");
        if(create && ftruncate(fd,sizeof(Shared)))throw std::runtime_error("ftruncate failed");
        struct stat st{};
        if(fstat(fd,&st)||st.st_size!=sizeof(Shared))throw std::runtime_error("Invalid shared mapping size");
        void* p=mmap(nullptr,sizeof(Shared),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
        if(p==MAP_FAILED)throw std::runtime_error("mmap failed");
        shared=static_cast<Shared*>(p);
        if(create)new(shared) Shared{};
    }
    ~Mapping(){if(shared)munmap(shared,sizeof(Shared));if(fd>=0)close(fd);}
    Mapping(const Mapping&)=delete;
};
inline void report_error(Shared& s,const std::string& text){
    std::strncpy(s.host.error_message,text.c_str(),sizeof(s.host.error_message)-1);release(&s.host.error,1);
}
inline void check_peer(const Shared& s){if(acquire(&s.host.error))throw std::runtime_error(s.host.error_message);}
inline void require_ok(const ControlResult& r,const char* name){if(!r.ok())throw std::runtime_error(std::string(name)+": "+r.describe());}
}
