#pragma once
#include <stdexcept>
#include <string>
namespace ap {
enum class Trigger { None, PreemptWait, PreemptAsync, Restart, Disable, DisableSplit };
struct ModePlan {
    std::string name;
    bool background=true,identity=false,timeslice=false,realtime=false,extended=false;
    Trigger operation=Trigger::None;
    Trigger trigger(bool identity_valid,bool realtime_configured)const{
        if(identity&&!identity_valid)throw std::runtime_error("OBJECT_BINDING_UNAVAILABLE: active mode requires owned identity");
        if(realtime&&!realtime_configured)throw std::runtime_error("INVALID_OBJECT_OR_STATE: realtime configuration not accepted");
        return operation;
    }
};
inline ModePlan mode_plan(std::string name){
    if(name=="realtime")name="realtime-restart";
    ModePlan p;p.name=name;
    if(name=="int-only")p.background=false;
    else if(name=="none"){}
    else if(name=="timeslice")p.identity=p.timeslice=true;
    else if(name=="realtime-only"||name=="realtime-restart"){
        p.identity=p.realtime=true;if(name=="realtime-restart")p.operation=Trigger::Restart;
    }else if(name=="preempt-wait"){p.identity=true;p.operation=Trigger::PreemptWait;}
    else if(name=="preempt-async"){p.identity=p.extended=true;p.operation=Trigger::PreemptAsync;}
    else if(name=="disable"||name=="disable-split"){
        p.identity=p.extended=true;p.operation=name=="disable"?Trigger::Disable:Trigger::DisableSplit;
    }else throw std::invalid_argument("Unknown mode: "+name);
    return p;
}
}
