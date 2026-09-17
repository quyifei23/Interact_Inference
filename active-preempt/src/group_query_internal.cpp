#include "group_query_internal.h"
#include <nvos.h>
#include <nv_escape.h>
#include <ctrl/ctrla06c.h>
#include <class/cl2080_notification.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cerrno>

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
    consumed_=true; // one request per probe process, including failed syscalls
    NVOS54_PARAMETERS args{};args.hClient=b.client;args.hObject=b.group.token.handle;
    args.cmd=NVA06C_CTRL_CMD_GET_INFO;args.params=&params;args.paramsSize=sizeof(params);args.status=0xffffffff;
    r.begin_ns=monotonic_ns();r.attempted=true;event->result.begin_ns=r.begin_ns;
    ++state.project_controls_attempted;++state.project_readonly_controls_attempted;
    errno=0;r.syscall_result=transport(b.fd,_IOWR('F',NV_ESC_RM_CONTROL,NVOS54_PARAMETERS),&args,context);
    r.syscall_errno=r.syscall_result<0?errno:0;r.end_ns=monotonic_ns();r.rm_status=args.status;
    if(r.ok()){
        out.hardware_tsg_id=params.tsgID;event->tsg_id=params.tsgID;
        state.group_get_info_verified=true; // never changes channel/active facts
    }
    journal.complete(event,r);return out;
}
}
