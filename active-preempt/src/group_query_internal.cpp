#include "group_query_internal.h"
#include <nvos.h>
#include <nv_escape.h>
#include <ctrl/ctrla06c.h>
#include <class/cl2080_notification.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace ap::detail {
bool group_engine_reviewed(const GroupBinding& b){
    // Keep unspecified engines unspecified. Parent engine is not copied into
    // channel records. Only explicit values are checked against this SDK.
    if(b.group.engine&&!NV2080_ENGINE_TYPE_IS_GR(b.group.engine))return false;
    for(const auto& m:b.members){
        if(m.channel.engine&&!NV2080_ENGINE_TYPE_IS_GR(m.channel.engine))return false;
        if(m.channel.engine&&b.group.engine&&m.channel.engine!=b.group.engine)return false;
    }
    return true;
}
GroupInfoResult GroupInfoOnce::query(const ObjectRegistry& registry,ProfileState& state,const GroupBinding& b,
                                    bool fd_valid,ControlJournal& journal,int64_t trial,GroupIoctl transport,void* context){
    static_assert(sizeof(NVA06C_CTRL_GET_INFO_PARAMS)==4&&alignof(NVA06C_CTRL_GET_INFO_PARAMS)==4);
    GroupInfoResult out;auto& r=out.control;
    NVA06C_CTRL_GET_INFO_PARAMS params{};
    auto* event=journal.begin(nullptr,b.group.token.handle,NVA06C_CTRL_CMD_GET_INFO,&params,sizeof(params),trial,&b);
    using Rejection=ControlResult::Rejection;
    r.operation_seq=event?event->sequence:0;
    const auto& p=build_profile();
    if(!event)r.rejection=Rejection::LogFull;
    else if(consumed_)r.rejection=Rejection::AlreadyQueried;
    else if(!state.observation_enabled||b.profile_version!=p.version||b.source_commit!=p.source_commit||!version_matches(state.runtime_version,p.version))r.rejection=Rejection::Abi;
    else if(b.owner_pid!=uint32_t(getpid())||!registry.valid_group(b)||!group_engine_reviewed(b))r.rejection=Rejection::Binding;
    else if(!state.may_group_info())r.rejection=Rejection::Stage;
    else if(!fd_valid)r.rejection=Rejection::Device;
    if(r.rejection!=Rejection::None){journal.complete(event,r);return out;}
    // Allocate the evidence snapshot before the control returns, so recording
    // an accepted ioctl never depends on a subsequent vector/string allocation.
    verified_binding_=b;verified_uuid_=state.scope_gpu_uuid;verified_visible_=state.scope_visible_devices;
    consumed_=true; // one request per process, including failed syscalls
    NVOS54_PARAMETERS args{};args.hClient=b.client;args.hObject=b.group.token.handle;
    args.cmd=NVA06C_CTRL_CMD_GET_INFO;args.params=&params;args.paramsSize=sizeof(params);args.status=0xffffffff;
    r.begin_ns=monotonic_ns();r.attempted=true;event->result.begin_ns=r.begin_ns;
    ++state.project_controls_attempted;++state.project_readonly_controls_attempted;++state.project_group_get_info_attempted;
    errno=0;r.syscall_result=transport(b.fd,_IOWR('F',NV_ESC_RM_CONTROL,NVOS54_PARAMETERS),&args,context);
    r.syscall_errno=r.syscall_result<0?errno:0;r.end_ns=monotonic_ns();r.rm_status=args.status;
    if(r.ok()){
        out.hardware_tsg_id=params.tsgID;event->tsg_id=params.tsgID;tsg_id_=params.tsgID;
        state.group_get_info_verified=true; // never changes channel/active facts
    }
    journal.complete(event,r);return out;
}
bool GroupInfoOnce::verified_for(const GroupBinding& b,const ProfileState& s)const{
    return tsg_id_.has_value()&&verified_binding_&&b==*verified_binding_&&
        verified_uuid_==s.scope_gpu_uuid&&verified_visible_==s.scope_visible_devices;
}
ControlResult GroupPreemptOnce::preempt(const ObjectRegistry& registry,ProfileState& state,const GroupBinding& b,const GroupInfoOnce& info,
                                      bool fd_valid,ControlJournal& journal,int64_t trial,uint32_t timeout_us,GroupIoctl transport,void* context,bool environment_current){
    static_assert(sizeof(NVA06C_CTRL_PREEMPT_PARAMS)==8&&alignof(NVA06C_CTRL_PREEMPT_PARAMS)==4);
    static_assert(offsetof(NVA06C_CTRL_PREEMPT_PARAMS,bWait)==0&&offsetof(NVA06C_CTRL_PREEMPT_PARAMS,bManualTimeout)==1&&offsetof(NVA06C_CTRL_PREEMPT_PARAMS,timeoutUs)==4);
    NVA06C_CTRL_PREEMPT_PARAMS params;std::memset(&params,0,sizeof(params));
    params.bWait=NV_TRUE;params.bManualTimeout=NV_TRUE;params.timeoutUs=timeout_us;
    ControlResult r;using Rejection=ControlResult::Rejection;
    auto* event=journal.begin(nullptr,b.group.token.handle,NVA06C_CTRL_CMD_PREEMPT,&params,sizeof(params),trial,&b);
    r.operation_seq=event?event->sequence:0;const auto& profile=build_profile();
    if(!event)r.rejection=Rejection::LogFull;
    else if(consumed_)r.rejection=Rejection::AlreadyPreempted;
    else if(!timeout_us||timeout_us>NVA06C_CTRL_CMD_PREEMPT_MAX_MANUAL_TIMEOUT_US)r.rejection=Rejection::TimeoutRange;
    else if(state.stage!=RmStage::GroupActive||trial!=0)r.rejection=Rejection::Stage;
    else if(!state.active_experiment_authorized)r.rejection=Rejection::Authorization;
    else if(!state.observation_enabled||b.profile_version!=profile.version||b.source_commit!=profile.source_commit||!version_matches(state.runtime_version,profile.version))r.rejection=Rejection::Abi;
    else if(!environment_current||b.owner_pid!=uint32_t(getpid())||!registry.valid_group(b)||!group_engine_reviewed(b))r.rejection=Rejection::Binding;
    else if(!fd_valid)r.rejection=Rejection::Device;
    else if(!info.verified_for(b,state))r.rejection=Rejection::Readonly;
    else if(state.group_owner!=GroupOwner::Background)r.rejection=Rejection::Owner;
    else if(!state.workload_gpu_reviewed||state.authorized_visible_devices.empty()||state.authorized_visible_devices!=state.scope_visible_devices)r.rejection=Rejection::GpuScope;
    else if(!state.may_group_preempt())r.rejection=Rejection::Stage;
    if(event&&info.verified_for(b,state))event->tsg_id=*info.hardware_tsg_id();
    if(r.rejection!=Rejection::None){journal.complete(event,r);return r;}
    consumed_=true;
    NVOS54_PARAMETERS args{};args.hClient=b.client;args.hObject=b.group.token.handle; // never hardware TSG ID
    args.cmd=NVA06C_CTRL_CMD_PREEMPT;args.params=&params;args.paramsSize=sizeof(params);args.status=0xffffffff;
    r.begin_ns=monotonic_ns();r.attempted=true;event->result.begin_ns=r.begin_ns;
    ++state.project_controls_attempted;++state.project_active_controls_attempted;++state.project_group_preempt_attempted;
    errno=0;r.syscall_result=transport(b.fd,_IOWR('F',NV_ESC_RM_CONTROL,NVOS54_PARAMETERS),&args,context);
    r.syscall_errno=r.syscall_result<0?errno:0;r.end_ns=monotonic_ns();r.rm_status=args.status;
    journal.complete(event,r); // published before any CUDA drain/wait or cleanup
    return r;
}
}
