// CPU-only profile/transport tests. Invalid payload addresses must never be read.
#include "driver_profile.h"
#include "rm_observation.h"
#include "object_registry.h"
#include <nvos.h>
#include <nv_escape.h>
#include <ctrl/ctrla06c.h>
#include <sys/ioctl.h>
#include <iostream>
#include <stdexcept>
namespace {void need(bool value,const char* why){if(!value)throw std::runtime_error(why);}}
int main(){try{
    ap::DriverProfile p550{"550.120","5e52edb2034de7db4d8ae368dbc7c26b416bfa16","synthetic SDK path",false};
    ap::DriverProfile p595{"595.58.03","db0c4e65c8e34c678d745ddb1317f53f90d1072b","synthetic SDK path",true};
    for(const auto& profile:{p550,p595}){
        ap::ProfileState s;s.configure(profile,std::string("Kernel Module ")+profile.version,ap::RmStage::Observe,false);
        need(s.static_abi_reviewed&&s.observation_enabled,"reviewed matching observe profile unavailable");
        s.cuda_minimal_workload_passed=s.binding_observed=s.workload_gpu_reviewed=true;
        need(!s.may_readonly()&&!s.may_active()&&!s.project_controls_attempted,"observe authorized a project control");
        ap::ProfileState ro;ro.configure(profile,profile.version,ap::RmStage::Readonly,false);
        ro.cuda_minimal_workload_passed=ro.binding_observed=ro.readonly_verified=ro.workload_gpu_reviewed=true;
        need(ro.may_readonly()&&!ro.may_active()&&!ro.active_experiment_authorized,"readonly granted active permission");
        ap::ProfileState active;active.configure(profile,profile.version,ap::RmStage::Active,true);
        active.cuda_minimal_workload_passed=active.binding_observed=active.workload_gpu_reviewed=true;
        need(!active.may_active(),"active skipped current readonly verification");
        active.readonly_verified=true;need(active.may_active()&&!active.active_result_measured,"first authorized active smoke requires a prior active success");
        for(auto runtime:{"unknown","615.71.09","595.58.03.1","550.120.1"}){ap::ProfileState bad;bad.configure(profile,runtime,ap::RmStage::Observe,false);need(!bad.observation_enabled,"unreviewed runtime enabled decoding");}
    }
    ap::ProfileState mismatch;mismatch.configure(p550,"595.58.03",ap::RmStage::Readonly,false);need(!mismatch.observation_enabled,"mixed build/runtime");
    auto request=_IOWR('F',NV_ESC_RM_ALLOC,NVOS64_PARAMETERS);
    auto hidden=ap::decode_observation(request,reinterpret_cast<void*>(1),0,0,false);
    need(!hidden.envelope_decoded,"disabled adapter dereferenced private payload");
    NVOS64_PARAMETERS serialized{};serialized.hClass=0xa06c;serialized.flags=NVOS64_FLAGS_FINN_SERIALIZED;serialized.pAllocParms=reinterpret_cast<void*>(1);
    auto unsupported=ap::decode_observation(request,&serialized,0,0,true);
    need(unsupported.action==ap::ObservedAction::Unsupported&&unsupported.flags==NVOS64_FLAGS_FINN_SERIALIZED,"FINN decoded as plain allocation");
    serialized.flags=0x80000000;need(ap::decode_observation(request,&serialized,0,0,true).action==ap::ObservedAction::Unsupported,"unknown flags accepted");
    need(ap::decode_observation(_IOC(_IOC_READ|_IOC_WRITE,'F',NV_ESC_RM_ALLOC,7),reinterpret_cast<void*>(1),0,0,true).action==ap::ObservedAction::Unsupported,"unknown envelope read");
    NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS params{};params.engineType=1;NVOS21_PARAMETERS plain{};
    plain.hRoot=1;plain.hObjectNew=30;plain.hObjectParent=10;plain.hClass=0xa06c;plain.paramsSize=sizeof(params);plain.pAllocParms=&params;
    auto decoded=ap::decode_observation(_IOWR('F',NV_ESC_RM_ALLOC,NVOS21_PARAMETERS),&plain,0,0,true);
    need(decoded.action==ap::ObservedAction::Allocate&&decoded.object==30&&decoded.parent==10&&decoded.engine==1,"plain reviewed allocation not decoded");
    ap::ObjectRegistry registry;registry.incomplete(unsupported.reason);bool refused=false;try{registry.discover();}catch(const std::exception&){refused=true;}need(refused,"unknown transport produced guessed binding");
    // A new process/registry cannot authenticate a historical snapshot.
    ap::ObjectRegistry empty;ap::Binding history;history.client=1;history.group={30,4};need(!empty.valid(history),"historical identity treated as current");
    // Shape seen during bring-up, with synthetic handles: eight compute
    // channels in one TSG. Preserve the refusal; never select the last one.
    ap::ObjectRegistry multiple;multiple.client(1,99);multiple.allocate(1,10,1,0x80);multiple.allocate(1,20,10,0x2080);multiple.allocate(1,30,10,0xa06c);
    for(unsigned i=0;i<8;++i){multiple.allocate(1,100+2*i,30,0xc56f);multiple.allocate(1,101+2*i,100+2*i,0xc6c0);}
    refused=false;try{multiple.discover();}catch(const std::exception& e){refused=std::string(e.what()).find("multiple compute channels")!=std::string::npos;}
    need(refused,"eight-channel real trace shape lost ambiguity rejection");
    std::cout<<"Profile "<<ap::build_profile().version<<": matching stages, no implicit authorization, unknown ABI/FINN and historical identity gates passed; no GPU exercised\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
