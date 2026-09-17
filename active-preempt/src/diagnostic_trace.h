#pragma once
#include <cstdint>
#include <string>

namespace ap {
// Optional host annotations only: no CUDA/RM calls, no kernel instrumentation.
// Initialize after RM stage selection, and in each owner after fork/exec.
void diagnostic_init(bool enabled,const std::string& run_id,const char* role,const std::string& directory);
void diagnostic_mark(const char* name,uint64_t operation_seq=0);
void diagnostic_save(); // outside the critical path, including error cleanup
class DiagnosticRange {
public:
    explicit DiagnosticRange(const char* name,uint64_t operation_seq=0);
    ~DiagnosticRange();
    DiagnosticRange(const DiagnosticRange&)=delete;
private:
    int record_=-1;
};
}
