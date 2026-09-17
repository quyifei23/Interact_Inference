#include "driver_profile.h"
#include "build_profile.h"
#include <sstream>
#include <class/cl0080.h>
#include <class/cl2080.h>
#include <class/cla06c.h>
#include <class/clc36f.h>
#include <class/clc46f.h>
#include <class/clc56f.h>
#include <class/clc86f.h>
#include <class/clc5c0.h>
#include <class/clc6c0.h>
#include <class/clc7c0.h>
// Registry class numbers must agree with THIS build's SDK, not another tag.
static_assert(NV01_DEVICE_0==0x80&&NV20_SUBDEVICE_0==0x2080&&KEPLER_CHANNEL_GROUP_A==0xa06c);
static_assert(VOLTA_CHANNEL_GPFIFO_A==0xc36f&&TURING_CHANNEL_GPFIFO_A==0xc46f&&AMPERE_CHANNEL_GPFIFO_A==0xc56f&&HOPPER_CHANNEL_GPFIFO_A==0xc86f);
static_assert(TURING_COMPUTE_A==0xc5c0&&AMPERE_COMPUTE_A==0xc6c0&&AMPERE_COMPUTE_B==0xc7c0);
namespace ap {
const DriverProfile& build_profile(){static const DriverProfile p{AP_RM_BUILD_VERSION,AP_RM_SOURCE_COMMIT,AP_RM_SOURCE_PATH,AP_RM_EXPERIMENTAL};return p;}
bool version_matches(const std::string& text,const std::string& version){
    std::istringstream stream(text);std::string token;while(stream>>token)if(token==version)return true;return false;
}
void ProfileState::configure(const DriverProfile& p,const std::string& runtime,RmStage requested,bool authorized){
    configured=true;stage=requested;runtime_version=runtime;
    static_abi_reviewed=(std::string(p.version)=="550.120"&&std::string(p.source_commit)=="5e52edb2034de7db4d8ae368dbc7c26b416bfa16")||
        (std::string(p.version)=="595.58.03"&&std::string(p.source_commit)=="db0c4e65c8e34c678d745ddb1317f53f90d1072b");
    runtime_matches=version_matches(runtime,p.version);
    observation_enabled=static_abi_reviewed&&runtime_matches&&stage!=RmStage::Disabled;
    active_experiment_authorized=stage==RmStage::Active&&authorized;
    if(!static_abi_reviewed||!runtime_matches)stop_reason="ABI_UNVERIFIED: runtime/build profile mismatch or unreviewed source";
}
bool ProfileState::may_readonly()const{return observation_enabled&&cuda_minimal_workload_passed&&binding_observed&&(stage==RmStage::Readonly||stage==RmStage::Active);}
bool ProfileState::may_active()const{return may_readonly()&&readonly_verified&&workload_gpu_reviewed&&active_experiment_authorized&&stage==RmStage::Active;}
}
