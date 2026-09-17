#include "rm_observation.h"
#include <nvos.h>
#include <nv_escape.h>
#include <ctrl/ctrla06c.h>
#include <sys/ioctl.h>
namespace ap {
IoctlObservation decode_observation(unsigned long request,const void* arg,int rc,int error,bool enabled){
    IoctlObservation e;e.type=_IOC_TYPE(request);e.number=_IOC_NR(request);e.size=_IOC_SIZE(request);e.syscall_result=rc;e.syscall_errno=error;
    if(!enabled){e.reason="observation_disabled_no_payload_decoded";return e;}
    if(rc||e.type!='F'||!arg){e.reason="syscall_failed_or_no_RM_envelope";return e;}
    auto unsupported=[&](const char* why){e.action=ObservedAction::Unsupported;e.reason=why;return e;};
    if(e.number==NV_ESC_RM_ALLOC){
        void* payload=nullptr;
        if(e.size==sizeof(NVOS21_PARAMETERS)){
            auto& a=*static_cast<const NVOS21_PARAMETERS*>(arg);e.client=a.hRoot;e.object=a.hObjectNew;e.parent=a.hObjectParent;e.cls=a.hClass;e.status=a.status;e.params_size=a.paramsSize;payload=a.pAllocParms;
        }else if(e.size==sizeof(NVOS64_PARAMETERS)){
            auto& a=*static_cast<const NVOS64_PARAMETERS*>(arg);e.client=a.hRoot;e.object=a.hObjectNew;e.parent=a.hObjectParent;e.cls=a.hClass;e.status=a.status;e.params_size=a.paramsSize;payload=a.pAllocParms;e.flags=a.flags;e.flags_present=true;
        }else return unsupported("unsupported_allocation_envelope");
        e.envelope_decoded=true;
        if(e.flags!=NVOS64_FLAGS_NONE)return unsupported(e.flags==NVOS64_FLAGS_FINN_SERIALIZED?"FINN_allocation_not_decoded":"unknown_allocation_flags");
        if(e.status!=NV_OK){e.reason="allocation_RM_rejected";return e;}
        if(e.cls==0xa06c){
            if(!payload||(e.params_size!=0&&e.params_size!=sizeof(NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS)))return unsupported("unsupported_TSG_allocation_layout");
            e.engine=static_cast<const NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS*>(payload)->engineType;
        }
        e.action=ObservedAction::Allocate;e.reason="plain_allocation";
    }else if(e.number==NV_ESC_RM_FREE){
        if(e.size!=sizeof(NVOS00_PARAMETERS))return unsupported("unsupported_FREE_layout");
        auto& a=*static_cast<const NVOS00_PARAMETERS*>(arg);e.envelope_decoded=true;e.client=a.hRoot;e.object=a.hObjectOld;e.parent=a.hObjectParent;e.status=a.status;
        if(e.status==NV_OK){e.action=ObservedAction::Free;e.reason="FREE";}
    }else if(e.number==NV_ESC_RM_CONTROL){
        if(e.size!=sizeof(NVOS54_PARAMETERS))return unsupported("unsupported_control_envelope");
        auto& a=*static_cast<const NVOS54_PARAMETERS*>(arg);e.envelope_decoded=true;e.client=a.hClient;e.object=a.hObject;e.command=a.cmd;e.flags=a.flags;e.flags_present=true;e.params_size=a.paramsSize;e.status=a.status;
        if(a.flags!=NVOS54_FLAGS_NONE)return unsupported(a.flags==NVOS54_FLAGS_FINN_SERIALIZED?"FINN_control_not_decoded":"unknown_control_flags");
        e.reason="normal_application_control_payload_not_inspected";
        if(a.status==NV_OK&&(a.cmd==NVA06C_CTRL_CMD_BIND||a.cmd==NVA06F_CTRL_CMD_BIND)){
            if(!a.params||a.paramsSize!=sizeof(NVA06F_CTRL_BIND_PARAMS))return unsupported("unsupported_BIND_layout");
            e.engine=static_cast<const NVA06F_CTRL_BIND_PARAMS*>(a.params)->engineType;e.action=ObservedAction::Bind;e.reason="BIND";
        }
    }else if(e.number==NV_ESC_RM_DUP_OBJECT||e.number==NV_ESC_RM_ALLOC_OBJECT)return unsupported("uncovered_object_allocation_or_duplication_transport");
    return e;
}
}
