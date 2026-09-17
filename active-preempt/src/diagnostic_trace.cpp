#include "diagnostic_trace.h"
#include "json_log.h"
#include "rm_control.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <unistd.h>
#if AP_HAVE_NVTX
#include <nvtx3/nvToolsExt.h>
#endif

namespace ap {
namespace {
struct Record { char label[256]{}; uint64_t seq=0,begin_before=0,begin_after=0,end_before=0,end_after=0; bool range=false; };
// Only the owner command thread writes; the independent observer is unchanged.
std::array<Record,128> records;
size_t count=0; bool enabled=false,overflow=false;
std::string prefix,directory,role;
int begin(const char* name,uint64_t seq,bool range){
    if(!enabled)return -1;
    if(count==records.size()){overflow=true;return -1;}
    int index=static_cast<int>(count++);auto& r=records[index];r.seq=seq;r.range=range;
    int n=std::snprintf(r.label,sizeof(r.label),"%s/%llu/%s",prefix.c_str(),static_cast<unsigned long long>(seq),name);
    if(n<0||size_t(n)>=sizeof(r.label)){overflow=true;return -1;}
    r.begin_before=monotonic_ns();
#if AP_HAVE_NVTX
    if(range)nvtxRangePushA(r.label);else nvtxMarkA(r.label);
#endif
    r.begin_after=monotonic_ns();return index;
}
}
void diagnostic_init(bool requested,const std::string& run_id,const char* owner,const std::string& dir){
    if(!requested)return;
#if !AP_HAVE_NVTX
    throw std::runtime_error("OBSERVABILITY_UNAVAILABLE: build has no NVTX headers");
#endif
    if(enabled||run_id.empty()||run_id.size()>100)throw std::runtime_error("Invalid diagnostic initialization/run ID");
    role=owner;directory=dir;prefix="AP/phase7/"+run_id+"/"+role+"/"+std::to_string(getpid());enabled=true;
    diagnostic_mark("owner_init"); // initialize NVTX injection before the trial
    diagnostic_save();
}
void diagnostic_mark(const char* name,uint64_t operation_seq){begin(name,operation_seq,false);}
DiagnosticRange::DiagnosticRange(const char* name,uint64_t operation_seq):record_(begin(name,operation_seq,true)){}
DiagnosticRange::~DiagnosticRange(){
    if(record_<0)return;
    auto& r=records[record_];r.end_before=monotonic_ns();
#if AP_HAVE_NVTX
    nvtxRangePop();
#endif
    r.end_after=monotonic_ns();
}
void diagnostic_save(){
    if(!enabled)return;
    std::ofstream f(directory+"/"+role+"_diagnostic_markers.jsonl");
    for(size_t i=0;i<count;++i){const auto& r=records[i];
        f<<"{\"run_kind\":\"diagnostic\",\"pid\":"<<getpid()<<",\"role\":"<<json_string(role)
         <<",\"record\":"<<i<<",\"label\":"<<json_string(r.label)<<",\"operation_seq\":"<<r.seq
         <<",\"kind\":"<<json_string(r.range?"range":"mark")<<",\"clock_domain\":\"CLOCK_MONOTONIC_RAW\""
         <<",\"begin_before_ns\":"<<r.begin_before<<",\"begin_after_ns\":"<<r.begin_after
         <<",\"end_before_ns\":"<<(r.end_before?std::to_string(r.end_before):"null")
         <<",\"end_after_ns\":"<<(r.end_after?std::to_string(r.end_after):"null")<<"}\n";
    }
    std::ofstream metadata(directory+"/"+role+"_diagnostic_environment.json");
    metadata<<"{\"run_kind\":\"diagnostic\",\"pid\":"<<getpid()<<",\"marker_overflow\":"<<(overflow?"true":"false")
            <<",\"LD_PRELOAD\":"<<json_string(std::getenv("LD_PRELOAD")?std::getenv("LD_PRELOAD"):"")
            <<",\"CUDA_INJECTION64_PATH\":"<<json_string(std::getenv("CUDA_INJECTION64_PATH")?std::getenv("CUDA_INJECTION64_PATH"):"")<<"}\n";
    std::ifstream maps("/proc/self/maps");std::ofstream loaded(directory+"/"+role+"_diagnostic_maps.txt");loaded<<maps.rdbuf();
    if(!f||!metadata||!loaded)std::fprintf(stderr,"DIAGNOSTIC_EXPORT_FAILED: preserve control journal\n");
}
}
