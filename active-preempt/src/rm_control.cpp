#include "rm_control.h"
#include "control_events.h"
#include "rm_observation.h"
#include "json_log.h"
#include <nvstatus.h>
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
#include <optional>
#include <set>
#include <cstdlib>
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
    ap::ProfileState state;
    std::optional<ap::Binding> observed_binding,verified_binding;
    std::vector<ap::IoctlObservation> observations;
    uint64_t early_ioctls=0;
    std::string journal_path;
    int64_t trial=-1;
};
// Process lifetime avoids destruction before libcuda's exit-time RM_FREE calls.
Capture& capture(){static Capture* c=new Capture;return *c;}
void observe(int fd,unsigned long request,void* arg,int rc,int error){
    auto& c=capture();auto& r=c.objects;
    if(!c.state.configured){++c.early_ioctls;return;}
    auto e=ap::decode_observation(request,arg,rc,error,c.state.observation_enabled);e.fd=fd;e.sequence=c.observations.size()+1;
    if(_IOC_NR(request)==NV_ESC_RM_CONTROL)++c.state.application_rm_controls_observed;
    if(c.observations.size()>=65536){r.incomplete("ioctl observation capacity exceeded");return;}
    c.observations.push_back(e);
    if(e.action==ap::ObservedAction::Unsupported){r.incomplete(e.reason);return;}
    if(e.action==ap::ObservedAction::Allocate){
        uint32_t client=e.client?e.client:e.object;
        if(!r.has_client(client)){
            char path[64],target[256];std::snprintf(path,sizeof(path),"/proc/self/fd/%d",fd);
            auto n=readlink(path,target,sizeof(target)-1);
            if(n<0){r.incomplete("cannot inspect allocating FD");return;}target[n]='\0';
            if(std::strcmp(target,"/dev/nvidiactl")!=0){r.incomplete("allocation not on original nvidiactl FD");return;}
            int held=fcntl(fd,F_DUPFD_CLOEXEC,3);
            if(held<0){r.incomplete("cannot retain allocating open-file");return;}
            r.client(client,held);
        }
        r.allocate(client,e.object,e.parent,e.cls,e.engine);
    }else if(e.action==ap::ObservedAction::Free){
        int held=e.client==e.object?r.client_fd(e.client):-1;
        r.free(e.client,e.object);if(held>=0)close(held);
    }else if(e.action==ap::ObservedAction::Bind)r.bind(e.client,e.object,e.engine);
}
bool readonly_command(uint32_t cmd){return cmd==NVA06C_CTRL_CMD_GET_INFO||cmd==NVA06C_CTRL_CMD_GET_TIMESLICE||cmd==NV2080_CTRL_CMD_GR_GET_CTXSW_MODES;}
ap::ControlResult issue(int fd,uint32_t client,uint32_t object,uint32_t cmd,void* params,uint32_t size,
                        const ap::Identity* id=nullptr,ap::ControlResult::Rejection rejection=ap::ControlResult::Rejection::None){
    auto& c=capture();std::lock_guard<std::recursive_mutex> lock(c.mutex);
    ap::ControlResult result;
    ap::ControlEvent* event=c.journaling?c.journal.begin(id,object,cmd,params,size,c.trial):nullptr;
    if(c.journaling&&!event)rejection=ap::ControlResult::Rejection::LogFull;
    if(id){
        if(!c.state.observation_enabled)rejection=ap::ControlResult::Rejection::Abi;
        else if(c.pid!=getpid()||!c.objects.valid(id->binding))rejection=ap::ControlResult::Rejection::Binding;
        else if(!c.state.may_readonly())rejection=ap::ControlResult::Rejection::Stage;
        else if(!readonly_command(cmd)){
            if(!c.state.active_experiment_authorized)rejection=ap::ControlResult::Rejection::Authorization;
            else if(!c.state.readonly_verified||!c.verified_binding||!(id->binding==*c.verified_binding))rejection=ap::ControlResult::Rejection::Readonly;
            else if(!c.state.workload_gpu_reviewed)rejection=ap::ControlResult::Rejection::GpuScope;
            else if(!c.state.may_active())rejection=ap::ControlResult::Rejection::Stage;
        }
    }
    result.operation_seq=event?event->sequence:0;
    if(rejection!=ap::ControlResult::Rejection::None){result.rejection=rejection;c.journal.complete(event,result);return result;}
    NVOS54_PARAMETERS a{};a.hClient=client;a.hObject=object;a.cmd=cmd;a.params=params;a.paramsSize=size;a.status=0xffffffff;
    result.begin_ns=ap::monotonic_ns();result.attempted=true;
    ++c.state.project_controls_attempted;if(!readonly_command(cmd))++c.state.project_active_controls_attempted;
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
    try{observe(fd,request,arg,rc,rc<0?saved:0);}catch(...){try{c.objects.incomplete("exception in ioctl capture");}catch(...){std::terminate();}}
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
    case Rejection::Stage:return "STAGE_REJECTED";
    case Rejection::Authorization:return "ACTIVE_NOT_AUTHORIZED";
    case Rejection::Readonly:return "READONLY_NOT_VERIFIED";
    case Rejection::GpuScope:return "WORKLOAD_GPU_UNREVIEWED";
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
    if(rm_status==NV_ERR_INSUFFICIENT_PERMISSIONS)return "PERMISSION_DENIED";
    if(rm_status==NV_ERR_INVALID_CLIENT)return "OWNERSHIP_REJECTED"; // invalid client; exact rejecting branch unknown
    if(rm_status==NV_ERR_NOT_SUPPORTED)return "CONTROL_NOT_SUPPORTED";
    if(rm_status==NV_ERR_TIMEOUT||rm_status==NV_ERR_TIMEOUT_RETRY)return "CONTROL_TIMEOUT";
    if(rm_status==0xffffffff)return "RM_STATUS_UNAVAILABLE";
    return "INVALID_OBJECT_OR_STATE";
}
std::string ControlResult::describe()const{std::ostringstream s;s<<category()<<" attempted="<<attempted<<" ioctl="<<syscall_result<<" errno="<<syscall_errno<<" rm_status=0x"<<std::hex<<rm_status;return s.str();}
bool profile_matches(const std::string& s){return version_matches(s,build_profile().version);}
std::string loaded_driver_version(){std::ifstream f("/proc/driver/nvidia/version");std::stringstream s;s<<f.rdbuf();return s.str();}
bool baseline_driver_loaded(){return profile_matches(loaded_driver_version());}
void configure_rm(RmStage stage,bool active_authorized){
    auto& c=capture();std::lock_guard<std::recursive_mutex> lock(c.mutex);
    if(c.state.configured)throw std::runtime_error("RM observation stage already configured; create a fresh process");
    c.state.configure(build_profile(),loaded_driver_version(),stage,active_authorized);
    c.observations.reserve(65536);
    if(c.early_ioctls)c.objects.incomplete("RM ioctls occurred before explicit stage selection; incomplete capture");
    if(stage!=RmStage::Disabled&&!c.state.observation_enabled)throw std::runtime_error(c.state.stop_reason);
}
void note_cuda_ready(int major,int minor){
    auto& c=capture();std::lock_guard<std::recursive_mutex> lock(c.mutex);
    c.state.cuda_minimal_workload_passed=true;c.state.workload_gpu_reviewed=major==8&&minor==0;
}
void note_active_result_measured(){
    auto& c=capture();std::lock_guard<std::recursive_mutex> lock(c.mutex);
    if(c.state.stage==RmStage::Active)c.state.active_result_measured=true;
}
void record_rm_stop_reason(const std::string& reason){auto& c=capture();std::lock_guard<std::recursive_mutex> lock(c.mutex);c.state.stop_reason=reason;}
std::string profile_state_json(){
    auto& c=capture();std::lock_guard<std::recursive_mutex> lock(c.mutex);const auto& s=c.state;const auto& p=build_profile();
    std::ostringstream out;out<<std::boolalpha<<"{\"build_profile\":"<<json_string(p.version)<<",\"source_commit\":"<<json_string(p.source_commit)
        <<",\"header_source\":"<<json_string(p.source_path)<<",\"experimental\":"<<p.experimental<<",\"pid\":"<<getpid()
        <<",\"runtime_kmd\":"<<json_string(s.runtime_version)<<",\"stage\":"<<int(s.stage)
        <<",\"static_abi_reviewed\":"<<s.static_abi_reviewed<<",\"runtime_matches\":"<<s.runtime_matches<<",\"observation_enabled\":"<<s.observation_enabled
        <<",\"cuda_minimal_workload_passed\":"<<s.cuda_minimal_workload_passed<<",\"workload_gpu_reviewed\":"<<s.workload_gpu_reviewed
        <<",\"binding_observed\":"<<s.binding_observed<<",\"current_binding_valid\":"<<(c.observed_binding&&c.objects.valid(*c.observed_binding))
        <<",\"readonly_verified\":"<<s.readonly_verified<<",\"active_experiment_authorized\":"<<s.active_experiment_authorized<<",\"active_result_measured\":"<<s.active_result_measured
        <<",\"application_rm_controls_observed\":"<<s.application_rm_controls_observed<<",\"project_controls_attempted\":"<<s.project_controls_attempted
        <<",\"project_active_controls_attempted\":"<<s.project_active_controls_attempted<<",\"early_unobserved_ioctls\":"<<c.early_ioctls
        <<",\"stop_reason\":"<<json_string(s.stop_reason)<<",\"cuda_visible_devices\":";
    const char* visible=std::getenv("CUDA_VISIBLE_DEVICES");out<<(visible?json_string(visible):"null");
    std::ifstream maps("/proc/self/maps");std::string line;std::set<std::string> libraries;
    while(std::getline(maps,line))if(line.find("libcuda")!=std::string::npos){auto at=line.find('/');if(at!=std::string::npos)libraries.insert(line.substr(at));}
    out<<",\"loaded_cuda_libraries\":[";bool first=true;for(const auto& name:libraries){if(!first)out<<',';out<<json_string(name);first=false;}out<<"]}";return out.str();
}
void save_rm_observation(const std::string& directory,const std::string& prefix){
    auto& c=capture();std::lock_guard<std::recursive_mutex> lock(c.mutex);
    std::ofstream state(directory+"/"+prefix+"profile_state.json");state<<profile_state_json()<<'\n';
    std::ofstream inventory(directory+"/"+prefix+"capture.txt");inventory<<c.objects.inventory();
    std::ofstream events(directory+"/"+prefix+"observed_ioctls.jsonl");
    for(const auto& e:c.observations){
        events<<"{\"sequence\":"<<e.sequence<<",\"pid\":"<<c.pid<<",\"fd\":"<<e.fd<<",\"type\":"<<e.type<<",\"number\":"<<e.number<<",\"size\":"<<e.size
            <<",\"syscall_return\":"<<e.syscall_result<<",\"errno\":"<<e.syscall_errno<<",\"envelope_decoded\":"<<(e.envelope_decoded?"true":"false")<<",\"reason\":"<<json_string(e.reason);
        if(e.envelope_decoded){events<<",\"hClient\":"<<e.client<<",\"hObject\":"<<e.object<<",\"parent\":"<<e.parent<<",\"class\":"<<e.cls<<",\"command\":"<<e.command<<",\"params_size\":"<<e.params_size<<",\"NV_STATUS\":"<<e.status<<",\"flags\":";if(e.flags_present)events<<e.flags;else events<<"null";}
        events<<"}\n";
    }
}
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
Binding inspect_owned_compute_group(){
    auto& c=capture();std::lock_guard<std::recursive_mutex> lock(c.mutex);
    if(c.pid!=getpid()||!c.state.observation_enabled)throw std::runtime_error("ABI_UNVERIFIED: current observation profile unavailable");
    auto b=c.objects.discover();c.observed_binding=b;c.state.binding_observed=true;return b;
}
Identity discover_owned_compute_group(){
    auto& c=capture();std::lock_guard<std::recursive_mutex> lock(c.mutex);
    Binding b=inspect_owned_compute_group();Identity id;id.binding=b;id.fd=b.fd;id.client=b.client;id.device=b.device.handle;
    id.group=b.group.handle;id.subdevice=b.subdevice.handle;id.compute_channel=b.compute_channel.handle;id.engine=b.engine;
    for(auto ch:b.channels)id.channels.push_back(ch.handle);
    if(id.engine&&!NV2080_ENGINE_TYPE_IS_GR(id.engine))throw std::runtime_error("OBJECT_BINDING_UNAVAILABLE: non-GR engine");
    NVA06C_CTRL_GET_INFO_PARAMS p{};auto r=issue(id.fd,id.client,id.group,NVA06C_CTRL_CMD_GET_INFO,&p,sizeof(p),&id);
    if(!r.ok())throw std::runtime_error(std::string("GET_INFO: ")+r.describe());
    id.tsg_id=p.tsgID;c.verified_binding=b;c.state.readonly_verified=true;return id;
}
RmControl::RmControl(Identity id):id_(std::move(id)){
    auto& c=capture();std::lock_guard<std::recursive_mutex> lock(c.mutex);
    const auto& b=id_.binding;
    bool mirrored=id_.fd==b.fd&&id_.client==b.client&&id_.device==b.device.handle&&id_.subdevice==b.subdevice.handle&&id_.group==b.group.handle&&id_.compute_channel==b.compute_channel.handle&&id_.engine==b.engine&&id_.channels.size()==b.channels.size();
    for(size_t i=0;mirrored&&i<b.channels.size();++i)mirrored=id_.channels[i]==b.channels[i].handle;
    if(!mirrored||!c.objects.valid(b)||!c.verified_binding||!(b==*c.verified_binding)||id_.tsg_id==0xffffffff)throw std::runtime_error("OBJECT_BINDING_UNAVAILABLE: stale/unverified identity");
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
