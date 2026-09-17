#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ap {
uint64_t monotonic_ns();
struct ControlResult {
    int syscall_result = -1;
    int syscall_errno = 0;
    uint32_t rm_status = 0xffffffff;
    uint64_t begin_ns = 0, end_ns = 0;
    bool ok() const { return syscall_result == 0 && rm_status == 0; }
    std::string describe() const;
};
struct Identity {
    int fd = -1; // duplicate of the allocating CUDA control FD, never a fresh open
    uint32_t client = 0, group = 0, subdevice = 0, compute_channel = 0;
    uint32_t tsg_id = 0xffffffff, engine = 0;
    std::vector<uint32_t> channels;
};

// Discovery accepts only allocations observed in this process. No externally
// supplied hClient/hObject, QUERY_GROUP, global enumeration, or privilege bypass.
Identity discover_owned_compute_group();
std::string capture_inventory();
bool baseline_driver_loaded();

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
private:
    Identity id_;
    ControlResult call(uint32_t object, uint32_t cmd, void* params, uint32_t size);
};

// Narrow offline contract checks, including syscall failure vs RM success.
void check_abi_contract();
ControlResult probe_bad_fd_for_test();
}
