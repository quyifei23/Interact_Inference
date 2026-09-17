#pragma once
#include <string>
#include <cstdint>
namespace ap {
enum class RmStage {Disabled,Observe,Readonly,Active,GroupInfo};
struct DriverProfile {const char* version;const char* source_commit;const char* source_path;bool experimental;};
const DriverProfile& build_profile();
bool version_matches(const std::string& text,const std::string& version);
// Evidence and permission are independent. No previous active success is
// required to authorize the first smoke, but neither observation nor GET_INFO
// can grant that authorization.
struct ProfileState {
    RmStage stage=RmStage::Disabled;
    bool configured=false,static_abi_reviewed=false,runtime_matches=false,observation_enabled=false;
    bool cuda_minimal_workload_passed=false,workload_gpu_reviewed=false;
    bool binding_observed=false,readonly_verified=false,active_experiment_authorized=false,active_result_measured=false;
    // The two legacy binding/readonly fields above apply ONLY to strict channel
    // identities. Group evidence must never satisfy their downstream gates.
    bool group_binding_observed=false,group_get_info_verified=false,single_gpu_scope_verified=false;
    std::string scope_gpu_uuid,scope_visible_devices;
    uint64_t application_rm_controls_observed=0,project_controls_attempted=0,project_readonly_controls_attempted=0,project_active_controls_attempted=0;
    std::string runtime_version,stop_reason;
    void configure(const DriverProfile&,const std::string& runtime,RmStage requested,bool authorized);
    bool may_readonly()const;
    bool may_active()const;
    bool may_group_info()const;
};
void configure_rm(RmStage stage,bool active_authorized=false);
void note_cuda_ready(int major,int minor);
void note_group_cuda_scope(const std::string& uuid_hex,int visible_device_count);
bool group_probe_visibility_matches(const std::string& visible,const std::string& uuid_hex,int count);
void note_active_result_measured();
void record_rm_stop_reason(const std::string& reason);
std::string profile_state_json();
void save_rm_observation(const std::string& directory,const std::string& prefix="");
}
