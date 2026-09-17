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
    bool verified_for(const GroupBinding&,const ProfileState&)const;
    std::optional<uint32_t> hardware_tsg_id()const{return tsg_id_;}
private:
    bool consumed_=false;
    std::optional<GroupBinding> verified_binding_;
    std::optional<uint32_t> tsg_id_;
    std::string verified_uuid_,verified_visible_;
};
// Single synchronous smoke: at most one PREEMPT per owner process (therefore
// at most one per trial). No async, retry, hold or resume methods.
class GroupPreemptOnce {
public:
    ControlResult preempt(const ObjectRegistry&,ProfileState&,const GroupBinding&,const GroupInfoOnce&,
                          bool retained_fd_valid,ControlJournal&,int64_t trial,uint32_t timeout_us,
                          GroupIoctl,void* context=nullptr,bool environment_current=true,OwnerActionTiming* timing=nullptr,bool target_completed=false);
    ControlResult prepare_noop(const ObjectRegistry&,ProfileState&,const GroupBinding&,const GroupInfoOnce&,
                          bool retained_fd_valid,ControlJournal&,int64_t trial,bool environment_current=true,OwnerActionTiming* timing=nullptr);
private:
    bool consumed_=false;
    ControlResult action(const ObjectRegistry&,ProfileState&,const GroupBinding&,const GroupInfoOnce&,
                         bool,ControlJournal&,int64_t,uint32_t,GroupIoctl,void*,bool,OwnerActionTiming*,bool,bool);
};
// Caller holds Capture::mutex over validation, syscall and journaling. This
// covers observed alloc/free/bind, not hidden/direct driver syscalls.
}
