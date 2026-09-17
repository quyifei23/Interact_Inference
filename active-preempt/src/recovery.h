#pragma once
#include "rm_control.h"
#include <ostream>
namespace ap {
// Flags are set BEFORE mutation: an error can still leave a partially applied
// operation. Recovery is owner-side, at most one attempt per dirty property.
struct Recovery {
    bool disabled=false,realtime=false,timeslice=false;
    uint64_t original_timeslice=0;
    template<class RM>bool restore(RM& rm,std::ostream& out){
        bool ok=true;
        if(disabled){auto r=rm.disable(false);out<<"ENABLE "<<r.describe()<<'\n';if(r.ok())disabled=false;else ok=false;}
        if(realtime){auto r=rm.realtime(false);out<<"DEMOTE "<<r.describe()<<'\n';if(r.ok())realtime=false;else ok=false;}
        if(timeslice){auto r=rm.timeslice(original_timeslice);out<<"RESTORE_TIMESLICE "<<r.describe()<<'\n';if(r.ok())timeslice=false;else ok=false;}
        return ok;
    }
};
}
