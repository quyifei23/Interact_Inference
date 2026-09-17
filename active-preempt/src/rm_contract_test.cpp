#include "rm_control.h"
#include <cerrno>
#include <iostream>
#include <stdexcept>
int main() {
    ap::check_abi_contract();
    auto r=ap::probe_bad_fd_for_test();
    if(r.ok() || r.syscall_result!=-1 || r.syscall_errno!=EBADF || r.end_ns<r.begin_ns) return 1;
    ap::ControlResult misleading;misleading.rm_status=0;
    if(misleading.ok()) return 2; // original GPreempt accidentally treats this as success
    try {ap::discover_owned_compute_group();return 3;} catch(const std::runtime_error&) {}
    std::cout<<"ABI, syscall failure, and missing-identity rejection passed; no GPU exercised\n";
}
