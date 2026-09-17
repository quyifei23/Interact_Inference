#include "rm_control.h"
#include "control_events.h"
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
#include <regex>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

namespace {
struct Capture {
    std::recursive_mutex mutex;
    ap::ObjectRegistry objects;
    pid_t pid=getpid();
    ap::ControlJournal journal;
    bool journaling=false;
    bool profile_verified=false;
    const bool capture_abi_supported=ap::baseline_driver_loaded();
    std::string journal_path;
    int64_t trial=-1;
    Capture(){if(!capture_abi_supported)objects.incomplete("ABI_UNVERIFIED: ioctl observation disabled for unreviewed driver profile");}
};
// Process lifetime avoids destruction before libcuda's exit-time RM_FREE calls.
Capture& capture(){static Capture* c=new Capture;return *c;}
void remember(int fd,uint32_t client,uint32_t handle,uint32_t parent,uint32_t cls,
              void* params,uint32_t size,bool serialized){
    auto& r=capture().objects;
    if(serialized){r.incomplete("unsupported FINN serialized allocation ABI");return;}
    if(!client)client=handle; // root-client allocation returns its handle in hObjectNew
    if(!r.has_client(client)){
        char path[64],target[256];std::snprintf(path,sizeof(path),"/proc/self/fd/%d",fd);
        auto n=readlink(path,target,sizeof(target)-1);
        if(n<0){r.incomplete("cannot inspect allocating FD");return;}target[n]='\0';
        if(std::strcmp(target,"/dev/nvidiactl")!=0){r.incomplete("allocation not observed on original nvidiactl FD");return;}
        int held=fcntl(fd,F_DUPFD_CLOEXEC,3);
        if(held<0){r.incomplete("cannot retain allocating open-file");return;}
        r.client(client,held);
    }
    uint32_t engine=0;
    if(cls==0xa06c){
        if(!params||(size!=0&&size!=sizeof(NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS))){r.incomplete("unknown TSG allocation layout");return;}
        // paramsSize=0 is the legacy NVOS21 class-sized allocation convention.
        engine=static_cast<NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS*>(params)->engineType;
    }
    r.allocate(client,handle,parent,cls,engine);
}
void observe(int fd,unsigned long request,void* arg,int rc){
    if(rc!=0||_IOC_TYPE(request)!='F'||!arg)return;
    // CUDA baselines may run on other versions, but must not decode private
    // allocation payloads using 550 headers on those versions.
    if(!capture().capture_abi_supported)return;
    auto& r=capture().objects;
    if(_IOC_NR(request)==NV_ESC_RM_ALLOC){
        if(_IOC_SIZE(request)==sizeof(NVOS21_PARAMETERS)){
            auto& a=*static_cast<NVOS21_PARAMETERS*>(arg);
            if(a.status==0)remember(fd,a.hRoot,a.hObjectNew,a.hObjectParent,a.hClass,a.pAllocParms,a.paramsSize,false);
        }else if(_IOC_SIZE(request)==sizeof(NVOS64_PARAMETERS)){
            auto& a=*static_cast<NVOS64_PARAMETERS*>(arg);
            if(a.status==0)remember(fd,a.hRoot,a.hObjectNew,a.hObjectParent,a.hClass,a.pAllocParms,a.paramsSize,a.flags!=0);
        }else r.incomplete("unsupported allocation ioctl layout");
    }else if(_IOC_NR(request)==NV_ESC_RM_FREE){
        if(_IOC_SIZE(request)!=sizeof(NVOS00_PARAMETERS)){r.incomplete("unsupported FREE layout");return;}
        auto& a=*static_cast<NVOS00_PARAMETERS*>(arg);if(a.status)return;
        int held=a.hRoot==a.hObjectOld?r.client_fd(a.hRoot):-1;
        r.free(a.hRoot,a.hObjectOld);if(held>=0)close(held);
    }else if(_IOC_NR(request)==NV_ESC_RM_CONTROL){
        if(_IOC_SIZE(request)!=sizeof(NVOS54_PARAMETERS)){r.incomplete("unsupported control observation layout");return;}
        auto& a=*static_cast<NVOS54_PARAMETERS*>(arg);if(a.status)return;
        if(a.cmd==NVA06C_CTRL_CMD_BIND||a.cmd==NVA06F_CTRL_CMD_BIND){
            if(a.paramsSize!=sizeof(NVA06F_CTRL_BIND_PARAMS)||!a.params||a.flags){r.incomplete("unsupported BIND ABI");return;}
            r.bind(a.hClient,a.hObject,static_cast<NVA06F_CTRL_BIND_PARAMS*>(a.params)->engineType);
        }
    }else if(_IOC_NR(request)==NV_ESC_RM_DUP_OBJECT)r.incomplete("RM_DUP_OBJECT relationship not captured");
}
ap::ControlResult issue(int fd,uint32_t client,uint32_t object,uint32_t cmd,void* params,uint32_t size,
                        const ap::Identity* id=nullptr,ap::ControlResult::Rejection rejection=ap::ControlResult::Rejection::None){
    auto& c=capture();std::lock_guard<std::recursive_mutex> lock(c.mutex);
    ap::ControlResult result;
    ap::ControlEvent* event=c.journaling?c.journal.begin(id,object,cmd,params,size,c.trial):nullptr;
    if(c.journaling&&!event)rejection=ap::ControlResult::Rejection::LogFull;
    if(id){
        if(!c.profile_verified)rejection=ap::ControlResult::Rejection::Abi;
        else if(c.pid!=getpid()||!c.objects.valid(id->binding))rejection=ap::ControlResult::Rejection::Binding;
    }
    result.operation_seq=event?event->sequence:0;
    if(rejection!=ap::ControlResult::Rejection::None){result.rejection=rejection;c.journal.complete(event,result);return result;}
    NVOS54_PARAMETERS a{};a.hClient=client;a.hObject=object;a.cmd=cmd;a.params=params;a.paramsSize=size;a.status=0xffffffff;
    result.begin_ns=ap::monotonic_ns();result.attempted=true;
    if(event)event->result.begin_ns=result.begin_ns;
    errno=0;result.syscall_result=static_cast<int>(syscall(SYS_ioctl,fd,_IOWR('F',NV_ESC_RM_CONTROL,NVOS54_PARAMETERS),&a));
    result.syscall_errno=result.syscall_result<0?errno:0;result.end_ns=ap::monotonic_ns();result.rm_status=a.status;
    // Persist to preallocated shared backing immediately, BEFORE any CUDA wait.
    c.journal.complete(event,result);return result;
}
}
extern "C" int ioctl(int fd,unsigned long request,...) noexcept {
    va_list args;va_start(args,request);void* arg=va_arg(args,void*);va_end(args);
    using Fn=int(*)(int,unsigned long,...);static Fn real=reinterpret_cast<Fn>(dlsym(RTLD_NEXT,"ioctl"));
    if(_IOC_TYPE(request)!='F')return real?real(fd,request,arg):static_cast<int>(syscall(SYS_ioctl,fd,request,arg));
    // Serialize observed alloc/free/bind syscalls and own controls, not only the
    // post-ioctl bookkeeping. Direct/hidden syscalls remain an explicit limitation.
    auto& c=capture();std::lock_guard<std::recursive_mutex> lock(c.mutex);
    int rc=real?real(fd,request,arg):static_cast<int>(syscall(SYS_ioctl,fd,request,arg));int saved=errno;
    try{observe(fd,request,arg,rc);}catch(...){try{c.objects.incomplete("exception in ioctl capture");}catch(...){std::terminate();}}
    errno=saved;return rc;
}
namespace ap {
uint64_t monotonic_ns(){timespec t{};clock_gettime(CLOCK_MONOTONIC_RAW,&t);return uint64_t(t.tv_sec)*1000000000ull+t.tv_nsec;}
const char* ControlResult::category()const{
    switch(rejection){
    case Rejection::Device:return "DEVICE_NOT_ACCESSIBLE";
    case Rejection::Abi:return "ABI_UNVERIFIED";
    case Rejection::Binding:case Rejection::Incomplete:return "OBJECT_BINDING_UNAVAILABLE";
    case Rejection::PendingAsync:return "INVALID_OBJECT_OR_STATE";
    case Rejection::LogFull:return "OBSERVABILITY_UNAVAILABLE";
    default:break;
    }
    if(ok())return "CONTROL_ACCEPTED_EFFECT_UNVERIFIED";
    if(!attempted)return "NOT_ISSUED";
    if(syscall_result<0){
        if(syscall_errno==EBADF||syscall_errno==ENOENT||syscall_errno==ENODEV)return "DEVICE_NOT_ACCESSIBLE";
        if(syscall_errno==EPERM||syscall_errno==EACCES)return "PERMISSION_DENIED";
        if(syscall_errno==ENOTTY||syscall_errno==ENOSYS)return "CONTROL_NOT_SUPPORTED";
        if(syscall_errno==ETIMEDOUT)return "CONTROL_TIMEOUT";
        return "IOCTL_FAILURE";
    }
    if(rm_status==0x1b)return "PERMISSION_DENIED";
    if(rm_status==0x23)return "OWNERSHIP_REJECTED"; // invalid client; exact rejecting branch unknown
    if(rm_status==0x56)return "CONTROL_NOT_SUPPORTED";
    if(rm_status==0x65||rm_status==0x66)return "CONTROL_TIMEOUT";
    if(rm_status==0xffffffff)return "RM_STATUS_UNAVAILABLE";
    return "INVALID_OBJECT_OR_STATE";
}
std::string ControlResult::describe()const{std::ostringstream s;s<<category()<<" attempted="<<attempted<<" ioctl="<<syscall_result<<" errno="<<syscall_errno<<" rm_status=0x"<<std::hex<<rm_status;return s.str();}
bool profile_matches(const std::string& s){return std::regex_search(s,std::regex("(^|[[:space:]])550\\.120([[:space:]]|$)"));}
std::string loaded_driver_version(){std::ifstream f("/proc/driver/nvidia/version");std::stringstream s;s<<f.rdbuf();return s.str();}
bool baseline_driver_loaded(){return profile_matches(loaded_driver_version());}
void record_trial(int64_t trial){auto& c=capture();std::lock_guard<std::recursive_mutex> lock(c.mutex);c.trial=trial;}
void open_control_journal(const std::string& directory,const std::string& owner){auto& c=capture();std::lock_guard<std::recursive_mutex> lock(c.mutex);c.journal_path=directory+"/"+owner+"_control_events.jsonl";c.journal.open(directory+"/"+owner+"_control_events.bin",owner);c.journaling=true;}
void save_control_journal(){
    auto& c=capture();std::lock_guard<std::recursive_mutex> lock(c.mutex);
    // Export is best effort; a full disk must not prevent owner-side restore.
    // The already-published mmap records remain the primary crash evidence.
    try{if(c.journaling)c.journal.save(c.journal_path);}
    catch(const std::exception& e){std::fprintf(stderr,"CONTROL_LOG_EXPORT_FAILED: %s; retain binary journal\n",e.what());}
}
std::string capture_inventory(){auto& c=capture();std::lock_guard<std::recursive_mutex> lock(c.mutex);return c.objects.inventory();}
Identity discover_owned_compute_group(){
    auto& c=capture();std::lock_guard<std::recursive_mutex> lock(c.mutex);
    c.profile_verified=baseline_driver_loaded();
    Binding b=c.objects.discover();Identity id;id.binding=b;id.fd=b.fd;id.client=b.client;id.device=b.device.handle;
    id.group=b.group.handle;id.subdevice=b.subdevice.handle;id.compute_channel=b.compute_channel.handle;id.engine=b.engine;
    for(auto ch:b.channels)id.channels.push_back(ch.handle);
    if(id.engine&&!NV2080_ENGINE_TYPE_IS_GR(id.engine))throw std::runtime_error("OBJECT_BINDING_UNAVAILABLE: non-GR engine");
    NVA06C_CTRL_GET_INFO_PARAMS p{};auto r=issue(id.fd,id.client,id.group,NVA06C_CTRL_CMD_GET_INFO,&p,sizeof(p),&id);
    if(!r.ok())throw std::runtime_error(std::string("GET_INFO: ")+r.describe());
    id.tsg_id=p.tsgID;return id;
}
RmControl::RmControl(Identity id):id_(std::move(id)){
    auto& c=capture();std::lock_guard<std::recursive_mutex> lock(c.mutex);
    const auto& b=id_.binding;
    bool mirrored=id_.fd==b.fd&&id_.client==b.client&&id_.device==b.device.handle&&id_.subdevice==b.subdevice.handle&&id_.group==b.group.handle&&id_.compute_channel==b.compute_channel.handle&&id_.engine==b.engine&&id_.channels.size()==b.channels.size();
    for(size_t i=0;mirrored&&i<b.channels.size();++i)mirrored=id_.channels[i]==b.channels[i].handle;
    if(!mirrored||!c.objects.valid(b)||id_.tsg_id==0xffffffff)throw std::runtime_error("OBJECT_BINDING_UNAVAILABLE: stale/unverified identity");
}
ControlResult RmControl::call(uint32_t object,uint32_t cmd,void* p,uint32_t n){return issue(id_.fd,id_.client,object,cmd,p,n,&id_);}
ControlResult RmControl::preempt(bool wait,uint32_t timeout) {
    if(timeout==0 || timeout>NVA06C_CTRL_CMD_PREEMPT_MAX_MANUAL_TIMEOUT_US) throw std::invalid_argument("PREEMPT timeout outside (0,1s]");
    NVA06C_CTRL_PREEMPT_PARAMS p{};p.bWait=wait;p.bManualTimeout=wait;p.timeoutUs=wait?timeout:0;
    auto rejected=!async_gate_.may_issue()?ControlResult::Rejection::PendingAsync:ControlResult::Rejection::None;
    auto r=issue(id_.fd,id_.client,id_.group,NVA06C_CTRL_CMD_PREEMPT,&p,sizeof(p),&id_,rejected);
    if(!wait&&r.attempted)async_gate_.submitted();
    return r;
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
