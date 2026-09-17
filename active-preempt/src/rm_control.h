#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "object_registry.h"
#include "driver_profile.h"

namespace ap {
struct AsyncPreemptGate {
    bool pending=false;
    bool may_issue()const{return !pending;}
    void submitted(){pending=true;}
    void drained(){pending=false;}
};
uint64_t monotonic_ns();
struct ControlResult {
    bool attempted = false;
    int syscall_result = -1;
    int syscall_errno = 0;
    uint32_t rm_status = 0xffffffff;
    uint64_t begin_ns = 0, end_ns = 0;
    uint64_t operation_seq = 0;
    enum class Rejection:uint32_t {None,Device,Abi,Binding,Incomplete,PendingAsync,LogFull,Stage,Authorization,Readonly,GpuScope} rejection=Rejection::None;
    bool ok() const { return attempted && syscall_result == 0 && rm_status == 0; }
    std::string describe() const;
    const char* category() const;
};
struct Identity {
    int fd = -1; // duplicate of the allocating CUDA control FD, never a fresh open
    uint32_t client = 0, device = 0, group = 0, subdevice = 0, compute_channel = 0;
    uint32_t tsg_id = 0xffffffff, engine = 0;
    std::vector<uint32_t> channels;
    Binding binding;
};

// Discovery accepts only allocations observed in this process. No externally
// supplied hClient/hObject, QUERY_GROUP, global enumeration, or privilege bypass.
Identity discover_owned_compute_group();
Binding inspect_owned_compute_group(); // observation only, never issues GET_INFO
std::string capture_inventory();
bool baseline_driver_loaded();
bool profile_matches(const std::string& version_text);
std::string loaded_driver_version();
void record_trial(int64_t trial);
void open_control_journal(const std::string& directory,const std::string& owner);
void save_control_journal();

class RmControl {
public:
    explicit RmControl(Identity identity);
    const Identity& identity() const { return id_; }
    ControlResult preempt(bool wait, uint32_t timeout_us = 1000000);
    ControlResult realtime(bool enable);
    ControlResult restart(bool force, bool bypass_wait);
    ControlResult timeslice(uint64_t us);
    ControlResult get_timeslice(uint64_t& us);
    ControlResult disable(bool disable, bool only_scheduling = false);
    ControlResult get_preemption_mode(uint32_t& compute_mode);
    void mark_work_drained() { async_gate_.drained(); }
private:
    Identity id_;
    AsyncPreemptGate async_gate_;
    ControlResult call(uint32_t object, uint32_t cmd, void* params, uint32_t size);
};

// Narrow offline contract checks, including syscall failure vs RM success.
void check_abi_contract();
ControlResult probe_bad_fd_for_test();
}
