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

namespace ap {
constexpr uint32_t max_samples=65536, max_blocks=4096;
constexpr uint64_t gpu_watchdog_ns=2000000000ull, host_timeout_ns=15000000000ull;
inline uint32_t acquire(const uint32_t* p){return __atomic_load_n(p,__ATOMIC_ACQUIRE);}
inline void release(uint32_t* p,uint32_t v){__atomic_store_n(p,v,__ATOMIC_RELEASE);}
inline void relax(){asm volatile("pause" ::: "memory");}
inline void wait_value(const uint32_t* p,uint32_t value,const char* what) {
    uint64_t deadline=monotonic_ns()+host_timeout_ns;
    while(acquire(p)!=value){if(monotonic_ns()>deadline)throw std::runtime_error(std::string("Timeout: ")+what);relax();}
}
struct alignas(4096) Telemetry {
    alignas(64) uint32_t started=0;
    alignas(64) uint32_t done=0;
    alignas(64) uint32_t ping_request=0;
    alignas(64) uint32_t ping_ack=0;
    alignas(64) uint32_t heartbeat_count=0;
    uint32_t overflow=0;
    uint64_t start_gpu_ns=0, done_gpu_ns=0, ping_gpu_ns=0;
    uint64_t heartbeat_gpu_ns[max_samples]{};
    uint64_t cta_start_gpu_ns[max_blocks]{}, cta_end_gpu_ns[max_blocks]{};
    uint32_t watchdog[max_blocks]{};
};
enum class Command:uint32_t { None, Launch, Drain, PreemptWait, PreemptAsync, Disable, DisableScheduling, Enable, ReadMode, Exit };
struct alignas(4096) HostState {
    uint32_t ready=0,error=0,command_seq=0,ack_seq=0;
    Command command=Command::None;
    uint32_t bg_correct=0,bg_pid=0,bg_tsg=0xffffffff,bg_client=0,bg_group=0,bg_engine=0;
    uint32_t bg_mode_before=0xffffffff,bg_mode_after=0xffffffff;
    uint32_t bg_mode_status_before=0xffffffff,bg_mode_status_after=0xffffffff;
    uint64_t bg_context=0,bg_iterations=0,bg_submit_begin=0,bg_submit_end=0;
    double bg_solo_us=0,bg_uninstrumented_us=0;
    ControlResult result{},preliminary_result{};
    char error_message[512]{},bg_uuid[64]{};
};
struct Shared { HostState host; Telemetry bg; Telemetry interactive; };
static_assert(sizeof(Telemetry)%4096==0 && sizeof(HostState)%4096==0);
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
