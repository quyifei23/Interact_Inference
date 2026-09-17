#pragma once
#include "mode_plan.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
namespace ap {
enum class Probe {None,Cuda,Observe,Identity,Readonly,GroupInfo};
struct Options {
    std::string mode="none",run_dir,shared_path;
    unsigned trials=1,waves=1;
    uint64_t bg_us=80000,int_us=300,heartbeat_ns=2000,timeslice_us=1,bg_iterations=0,int_iterations=0;
    bool force=false,bypass=false,graph=false,diagnostic=false,test_host=false,extended=false,graph_reviewed=false;
    Probe probe=Probe::None;
};
inline Options parse(int argc,char** argv){
    Options o;unsigned probes_seen=0;
    for(int i=1;i<argc;++i){std::string key=argv[i];
        if(key=="--probe"||key=="--probe-cuda"||key=="--probe-rm-observe"||key=="--probe-rm-identity"||key=="--probe-rm-readonly"||key=="--probe-rm-group-info")
            if(++probes_seen>1)throw std::invalid_argument("Select exactly one probe stage");
        if(key=="--graph")o.graph=true;
        else if(key=="--diagnostic-progress")o.diagnostic=true;
        else if(key=="--test-host-confirmed")o.test_host=true;
        else if(key=="--allow-extended")o.extended=true;
        else if(key=="--graph-evidence-reviewed")o.graph_reviewed=true;
        else if(key=="--probe"||key=="--probe-cuda")o.probe=Probe::Cuda;
        else if(key=="--probe-rm-observe")o.probe=Probe::Observe;
        else if(key=="--probe-rm-identity")o.probe=Probe::Identity;
        else if(key=="--probe-rm-readonly")o.probe=Probe::Readonly;
        else if(key=="--probe-rm-group-info")o.probe=Probe::GroupInfo;
        else if(key=="--help"){
            std::cout<<"int_worker --probe-cuda | --probe-rm-observe | --probe-rm-identity | --probe-rm-readonly | --probe-rm-group-info [--run-dir DIR]\n"
                     <<"Group-info requires CUDA_VISIBLE_DEVICES=<one full GPU UUID>; adds GET_INFO once, no other controls.\n"
                     <<"int_worker --run-dir DIR --mode int-only|none|timeslice|preempt-wait|group-preempt-wait|preempt-async|realtime-only|realtime-restart|realtime|disable|disable-split\n"
                     <<"[--trials 1] [--cta-waves 1..16] [--bg-iterations N] [--int-iterations N] [--bg-us 80000] [--int-us 300]\n"
                     <<"[--heartbeat-ns 2000] [--timeslice-us 1] [--force 0|1] [--bypass 0|1] [--diagnostic-progress]\n"
                     <<"Active modes require --test-host-confirmed. Async/force/bypass/D also require --allow-extended.\n"
                     <<"group-preempt-wait: one plain trial, fixed BG/INT iterations, one explicit GPU UUID; BG owner only.\n"
                     <<"Graph requires --graph --graph-evidence-reviewed after independent plain-kernel evidence review. --probe aliases --probe-cuda.\n";
            std::exit(0);
        }else{
            if(++i>=argc)throw std::invalid_argument("Missing value for "+key);std::string value=argv[i];
            if(key=="--run-dir")o.run_dir=value;else if(key=="--shared")o.shared_path=value;else if(key=="--mode")o.mode=value;
            else{size_t used=0;uint64_t v=std::stoull(value,&used);if(used!=value.size()||value.empty()||value[0]=='-')throw std::invalid_argument("Invalid numeric option");
                if(key=="--trials"){if(v>100000)throw std::invalid_argument("trials > 100000");o.trials=v;}
                else if(key=="--cta-waves"){if(v>16)throw std::invalid_argument("waves > 16");o.waves=v;}
                else if(key=="--bg-us")o.bg_us=v;else if(key=="--int-us")o.int_us=v;
                else if(key=="--bg-iterations")o.bg_iterations=v;else if(key=="--int-iterations")o.int_iterations=v;
                else if(key=="--heartbeat-ns")o.heartbeat_ns=v;else if(key=="--timeslice-us")o.timeslice_us=v;
                else if(key=="--force"||key=="--bypass"){if(v>1)throw std::invalid_argument("Boolean requires 0 or 1");if(key=="--force")o.force=v;else o.bypass=v;}
                else throw std::invalid_argument("Unknown option "+key);
            }
        }
    }
    auto p=mode_plan(o.mode);o.mode=p.name;
    if(!o.trials||!o.waves||o.bg_us<50000||o.bg_us>100000||o.int_us<100||o.int_us>500||o.heartbeat_ns>1000000||!o.timeslice_us||o.timeslice_us>1000000)
        throw std::invalid_argument("Invalid workload/trial bounds");
    for(auto n:{o.bg_iterations,o.int_iterations})if(n&&(n%256||n>1000000000))throw std::invalid_argument("iterations must be multiples of 256 <= 1e9");
    if(o.probe==Probe::None){
        if((p.identity||p.group_identity)&&!o.test_host)throw std::invalid_argument("Active mode requires --test-host-confirmed (isolated authorized GPU host)");
        if(p.group_identity&&(o.trials!=1||o.graph||o.force||o.bypass||o.extended||o.diagnostic||!o.bg_iterations||!o.int_iterations))
            throw std::invalid_argument("group-preempt-wait requires one plain trial and frozen BG/INT iterations; no extended/diagnostic options");
        if((p.extended||o.force||o.bypass)&&!o.extended)throw std::invalid_argument("Extended modes require --allow-extended after recovery/wait-path validation");
        if(o.graph&&!o.graph_reviewed)throw std::invalid_argument("Graph requires --graph-evidence-reviewed; RM acceptance alone is insufficient");
    }
    if(o.probe==Probe::GroupInfo&&(p.name!="none"||o.graph||o.diagnostic||o.force||o.bypass||o.test_host||o.extended||o.graph_reviewed||o.trials!=1))
        throw std::invalid_argument("Group-info is a single readonly probe; benchmark/active options are not applicable");
    return o;
}
}
