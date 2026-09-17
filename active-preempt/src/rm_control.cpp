#include "rm_control.h"
#include <nvos.h>
#include <nv_escape.h>
#include <ctrl/ctrla06c.h>
#include <ctrl/ctrl2080/ctrl2080fifo.h>
#include <ctrl/ctrl2080/ctrl2080gr.h>
#include <class/cl2080_notification.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <mutex>
#include <algorithm>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

namespace {
struct Object { uint32_t client, handle, parent, cls, engine; bool live; };
struct Client { uint32_t handle; int fd; bool live; };
struct Registry {
    std::mutex mutex;
    Object objects[4096]{};
    Client clients[128]{};
    size_t count = 0, clients_count = 0;
    bool overflow = false;
};
// Keep the registry alive through libcuda's process-exit destructors, which can
// still issue RM_FREE ioctls. The OS releases remaining FD references on exit.
Registry& registry() { static Registry* r=new Registry; return *r; }
bool relevant(uint32_t cls) {
    return cls == 0xa06c || cls == 0x2080 || cls == 0xc56f ||
           cls == 0xc36f || cls == 0xc46f || cls == 0xc86f ||
           cls == 0xc5c0 || cls == 0xc6c0 || cls == 0xc7c0;
}
bool compute_class(uint32_t cls) { return cls == 0xc5c0 || cls == 0xc6c0 || cls == 0xc7c0; }
bool channel_class(uint32_t cls) { return cls == 0xc36f || cls == 0xc46f || cls == 0xc56f || cls == 0xc86f; }
void remember(int fd, uint32_t client, uint32_t handle, uint32_t parent,
              uint32_t cls, void* params, uint32_t size, bool serialized) {
    if (!relevant(cls)) return;
    auto& r = registry();
    std::lock_guard<std::mutex> guard(r.mutex);
    if (r.count == 4096) { r.overflow = true; return; }
    bool known = false;
    for (size_t i=0; i<r.clients_count; ++i) if (r.clients[i].live && r.clients[i].handle == client) known = true;
    if (!known) {
        if (r.clients_count == 128) { r.overflow = true; return; }
        char fd_path[64],target[256];
        std::snprintf(fd_path,sizeof(fd_path),"/proc/self/fd/%d",fd);
        ssize_t n=readlink(fd_path,target,sizeof(target)-1);
        if(n<0) {r.overflow=true;return;}
        target[n]='\0';
        if(std::strcmp(target,"/dev/nvidiactl")!=0) {r.overflow=true;return;}
        // fcntl dup retains open-file identity used by secInfo.clientOSInfo.
        int held = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        if (held < 0) { r.overflow = true; return; }
        r.clients[r.clients_count++] = {client, held, true};
    }
    uint32_t engine = 0;
    if (cls == 0xa06c && params && !serialized &&
        (size == 0 || size >= sizeof(NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS)))
        engine = static_cast<NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS*>(params)->engineType;
    for (size_t i=0; i<r.count; ++i)
        if (r.objects[i].client == client && r.objects[i].handle == handle) r.objects[i].live = false;
    r.objects[r.count++] = {client,handle,parent,cls,engine,true};
}
void observe(int fd, unsigned long request, void* arg, int rc) {
    if (rc != 0 || _IOC_TYPE(request) != 'F' || arg == nullptr) return;
    if (_IOC_NR(request) == NV_ESC_RM_ALLOC) {
        if (_IOC_SIZE(request) == sizeof(NVOS21_PARAMETERS)) {
            auto& a = *static_cast<NVOS21_PARAMETERS*>(arg);
            if (a.status == 0) remember(fd,a.hRoot,a.hObjectNew,a.hObjectParent,a.hClass,a.pAllocParms,a.paramsSize,false);
        } else if (_IOC_SIZE(request) == sizeof(NVOS64_PARAMETERS)) {
            auto& a = *static_cast<NVOS64_PARAMETERS*>(arg);
            if (a.status == 0) remember(fd,a.hRoot,a.hObjectNew,a.hObjectParent,a.hClass,a.pAllocParms,a.paramsSize,a.flags != 0);
        }
    } else if (_IOC_NR(request) == NV_ESC_RM_FREE && _IOC_SIZE(request) == sizeof(NVOS00_PARAMETERS)) {
        const auto& a = *static_cast<NVOS00_PARAMETERS*>(arg);
        if (a.status != 0) return;
        auto& r=registry(); std::lock_guard<std::mutex> guard(r.mutex);
        // Invalidate descendants as well: group/device free can recursively free children.
        for (size_t pass=0; pass<=r.count; ++pass) {
            bool changed=false;
            for (size_t i=0;i<r.count;++i) {
                auto& o=r.objects[i];
                if (!o.live || o.client!=a.hRoot) continue;
                bool dead = a.hRoot==a.hObjectOld || o.handle==a.hObjectOld || o.parent==a.hObjectOld;
                for(size_t j=0;j<r.count && !dead;++j)
                    if(r.objects[j].client==o.client && r.objects[j].handle==o.parent && !r.objects[j].live) dead=true;
                if(dead) {o.live=false; changed=true;}
            }
            if(!changed) break;
        }
        if(a.hRoot==a.hObjectOld) for(size_t i=0;i<r.clients_count;++i)
            if(r.clients[i].live && r.clients[i].handle==a.hRoot) {close(r.clients[i].fd);r.clients[i].live=false;}
    } else if (_IOC_NR(request) == NV_ESC_RM_CONTROL && _IOC_SIZE(request) == sizeof(NVOS54_PARAMETERS)) {
        auto& a=*static_cast<NVOS54_PARAMETERS*>(arg);
        if(a.status != 0 || (a.cmd != NVA06C_CTRL_CMD_BIND && a.cmd != NVA06F_CTRL_CMD_BIND) ||
           a.paramsSize != sizeof(NVA06F_CTRL_BIND_PARAMS) || !a.params || a.flags) return;
        auto& r=registry(); std::lock_guard<std::mutex> guard(r.mutex);
        for(size_t i=0;i<r.count;++i) if(r.objects[i].live && r.objects[i].client==a.hClient && r.objects[i].handle==a.hObject)
            r.objects[i].engine=static_cast<NVA06F_CTRL_BIND_PARAMS*>(a.params)->engineType;
    }
}
ap::ControlResult issue(int fd,uint32_t client,uint32_t object,uint32_t cmd,void* params,uint32_t size) {
    NVOS54_PARAMETERS a{};
    a.hClient=client; a.hObject=object; a.cmd=cmd; a.params=params; a.paramsSize=size; a.status=0xffffffff;
    ap::ControlResult result;
    result.begin_ns=ap::monotonic_ns();
    errno=0;
    result.syscall_result=static_cast<int>(syscall(SYS_ioctl,fd,_IOWR('F',NV_ESC_RM_CONTROL,NVOS54_PARAMETERS),&a));
    result.syscall_errno=result.syscall_result<0 ? errno : 0;
    result.end_ns=ap::monotonic_ns(); result.rm_status=a.status;
    return result;
}
}

// Linux x86-64 CUDA ioctl interposition. It is deliberately best-effort:
// libcuda may use a hidden/direct syscall; discovery then fails closed.
extern "C" int ioctl(int fd, unsigned long request, ...) noexcept {
    va_list args; va_start(args,request); void* arg=va_arg(args,void*); va_end(args);
    using Fn=int(*)(int,unsigned long,...);
    static Fn real=reinterpret_cast<Fn>(dlsym(RTLD_NEXT,"ioctl"));
    int rc=real ? real(fd,request,arg) : static_cast<int>(syscall(SYS_ioctl,fd,request,arg));
    int saved=errno;
    try { observe(fd,request,arg,rc); } catch (...) { /* no exception crosses C ABI */ }
    errno=saved; return rc;
}

namespace ap {
uint64_t monotonic_ns() { timespec t{}; clock_gettime(CLOCK_MONOTONIC_RAW,&t); return uint64_t(t.tv_sec)*1000000000ull+t.tv_nsec; }
std::string ControlResult::describe() const { std::ostringstream s; s<<"ioctl="<<syscall_result<<" errno="<<syscall_errno<<" rm_status=0x"<<std::hex<<rm_status;return s.str(); }
bool baseline_driver_loaded() { std::ifstream f("/proc/driver/nvidia/version");std::stringstream s;s<<f.rdbuf();return s.str().find("550.120")!=std::string::npos; }
std::string capture_inventory() {
    auto& r=registry();std::lock_guard<std::mutex> g(r.mutex);std::ostringstream s;
    s<<"capture_count="<<r.count<<" overflow="<<r.overflow<<"\n";
    for(size_t i=0;i<r.count;++i) {auto& o=r.objects[i];if(o.live)s<<std::hex<<"client="<<o.client<<" handle="<<o.handle<<" parent="<<o.parent<<" class="<<o.cls<<" engine="<<o.engine<<"\n";}
    return s.str();
}
Identity discover_owned_compute_group() {
    Identity id;
    {
        auto& r=registry();std::lock_guard<std::mutex> guard(r.mutex);
        if(r.overflow) throw std::runtime_error("RM allocation registry overflow; refusing guessed handles");
        int matches=0;
        for(size_t i=0;i<r.count;++i) {
            const auto& group=r.objects[i];if(!group.live || group.cls!=0xa06c) continue;
            Identity candidate;candidate.client=group.client;candidate.group=group.handle;candidate.engine=group.engine;
            for(size_t j=0;j<r.count;++j) {
                const auto& channel=r.objects[j];
                if(!channel.live || channel.client!=group.client || channel.parent!=group.handle || !channel_class(channel.cls)) continue;
                candidate.channels.push_back(channel.handle);
                for(size_t k=0;k<r.count;++k) {
                    const auto& object=r.objects[k];
                    if(object.live && object.client==group.client && object.parent==channel.handle && compute_class(object.cls)) {
                        candidate.compute_channel=channel.handle;
                        if(!candidate.engine) candidate.engine=channel.engine;
                    }
                }
            }
            if(!candidate.compute_channel) continue;
            // A compute engine object child proves this is a compute group;
            // engine 0 is unspecified, never silently treated as a copy engine.
            if(candidate.engine && !NV2080_ENGINE_TYPE_IS_GR(candidate.engine)) continue;
            for(size_t j=0;j<r.count;++j) {const auto& o=r.objects[j];
                if(o.live && o.client==group.client && o.cls==0x2080 && o.parent==group.parent) {
                    if(candidate.subdevice) throw std::runtime_error("Multiple subdevices: prototype requires one physical GPU");
                    candidate.subdevice=o.handle;
                }
            }
            for(size_t j=0;j<r.clients_count;++j) if(r.clients[j].live && r.clients[j].handle==group.client) candidate.fd=r.clients[j].fd;
            id=candidate; ++matches;
        }
        if(matches!=1 || id.fd<0 || !id.subdevice || id.channels.empty())
            throw std::runtime_error("No unique captured owned compute TSG/subdevice. Direct/hidden ioctl, unsupported allocation ABI, MPS, or multiple contexts; no unsafe fallback.");
    }
    NVA06C_CTRL_GET_INFO_PARAMS p{};
    auto result=issue(id.fd,id.client,id.group,NVA06C_CTRL_CMD_GET_INFO,&p,sizeof(p));
    if(!result.ok()) throw std::runtime_error("GET_INFO rejected on original owned FD: "+result.describe());
    id.tsg_id=p.tsgID;return id;
}
RmControl::RmControl(Identity identity):id_(std::move(identity)) {
    auto actual=discover_owned_compute_group();
    if(id_.fd!=actual.fd || id_.client!=actual.client || id_.group!=actual.group ||
       id_.subdevice!=actual.subdevice || id_.compute_channel!=actual.compute_channel || id_.channels!=actual.channels)
        throw std::runtime_error("RmControl refuses externally supplied or stale identity");
}
ControlResult RmControl::call(uint32_t object,uint32_t cmd,void* p,uint32_t n) {return issue(id_.fd,id_.client,object,cmd,p,n);}
ControlResult RmControl::preempt(bool wait,uint32_t timeout) {
    if(timeout==0 || timeout>NVA06C_CTRL_CMD_PREEMPT_MAX_MANUAL_TIMEOUT_US) throw std::invalid_argument("PREEMPT timeout outside (0,1s]");
    NVA06C_CTRL_PREEMPT_PARAMS p{};p.bWait=wait;p.bManualTimeout=wait;p.timeoutUs=wait?timeout:0;
    return call(id_.group,NVA06C_CTRL_CMD_PREEMPT,&p,sizeof(p));
}
ControlResult RmControl::realtime(bool enable) {NVA06C_CTRL_MAKE_REALTIME_PARAMS p{};p.bRealtime=enable;return call(id_.group,NVA06C_CTRL_CMD_MAKE_REALTIME,&p,sizeof(p));}
ControlResult RmControl::restart(bool force,bool bypass) {NVA06F_CTRL_RESTART_RUNLIST_PARAMS p{};p.bForceRestart=force;p.bBypassWait=bypass;return call(id_.compute_channel,NVA06F_CTRL_CMD_RESTART_RUNLIST,&p,sizeof(p));}
ControlResult RmControl::timeslice(uint64_t us) {NVA06C_CTRL_TIMESLICE_PARAMS p{};p.timesliceUs=us;return call(id_.group,NVA06C_CTRL_CMD_SET_TIMESLICE,&p,sizeof(p));}
ControlResult RmControl::get_timeslice(uint64_t& us) {NVA06C_CTRL_TIMESLICE_PARAMS p{};auto r=call(id_.group,NVA06C_CTRL_CMD_GET_TIMESLICE,&p,sizeof(p));if(r.ok())us=p.timesliceUs;return r;}
ControlResult RmControl::disable(bool disable,bool only) {
    if(id_.channels.size()>NV2080_CTRL_FIFO_DISABLE_CHANNELS_MAX_ENTRIES) throw std::runtime_error("Too many channels; refusing partial disable");
    NV2080_CTRL_FIFO_DISABLE_CHANNELS_PARAMS p{};p.bDisable=disable;p.bOnlyDisableScheduling=only;
    p.bRewindGpPut=NV_FALSE;p.pRunlistPreemptEvent=nullptr;p.numChannels=id_.channels.size();
    for(size_t i=0;i<id_.channels.size();++i){p.hClientList[i]=id_.client;p.hChannelList[i]=id_.channels[i];}
    return call(id_.subdevice,NV2080_CTRL_CMD_FIFO_DISABLE_CHANNELS,&p,sizeof(p));
}
ControlResult RmControl::get_preemption_mode(uint32_t& mode) {
    NV2080_CTRL_GR_GET_CTXSW_MODES_PARAMS p{};p.hChannel=id_.compute_channel;
    auto r=call(id_.subdevice,NV2080_CTRL_CMD_GR_GET_CTXSW_MODES,&p,sizeof(p));
    if(r.ok()) mode=p.cilpPreemptMode;
    return r;
}
void check_abi_contract() {
    static_assert(sizeof(NVOS54_PARAMETERS)==32);
    static_assert(sizeof(NVA06C_CTRL_PREEMPT_PARAMS)==8);
    static_assert(sizeof(NVA06F_CTRL_RESTART_RUNLIST_PARAMS)==2);
    static_assert(sizeof(NVA06C_CTRL_MAKE_REALTIME_PARAMS)==1);
    static_assert(sizeof(NV2080_CTRL_FIFO_DISABLE_CHANNELS_PARAMS)==536);
    static_assert(_IOWR('F',NV_ESC_RM_CONTROL,NVOS54_PARAMETERS)==0xc020462aul);
}
ControlResult probe_bad_fd_for_test(){NVA06C_CTRL_PREEMPT_PARAMS p{};return issue(-1,0,0,NVA06C_CTRL_CMD_PREEMPT,&p,sizeof(p));}
}
