#pragma once
#include "control_events.h"

namespace ap::detail {
// Internal test seam. The request is constructed here from a GroupBinding;
// callers cannot supply a command or select a different target object.
using GroupIoctl = int(*)(int fd,unsigned long request,void* parameters,void* context);
bool group_engine_reviewed(const GroupBinding&);
class GroupInfoOnce {
public:
    GroupInfoResult query(const ObjectRegistry&,ProfileState&,const GroupBinding&,
                          bool retained_fd_valid,ControlJournal&,int64_t trial,
                          GroupIoctl,void* context=nullptr);
private:
    bool consumed_=false;
};
// Caller holds Capture::mutex over validation, syscall and journaling. This
// covers observed alloc/free/bind, not hidden/direct driver syscalls.
}
