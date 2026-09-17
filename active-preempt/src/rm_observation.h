#pragma once
#include <cstdint>
namespace ap {
enum class ObservedAction {Metadata,Allocate,Free,Bind,Unsupported};
// No opaque payload bytes are retained. Only known envelopes/known BIND and
// allocation layouts may be read; serialized payloads are never reinterpreted.
struct IoctlObservation {
    uint64_t sequence=0;int fd=-1,syscall_result=-1,syscall_errno=0;
    uint32_t type=0,number=0,size=0,flags=0,status=0xffffffff,client=0,object=0,parent=0,cls=0,command=0,params_size=0,engine=0;
    bool envelope_decoded=false,flags_present=false;
    ObservedAction action=ObservedAction::Metadata;
    const char* reason="metadata_only";
};
IoctlObservation decode_observation(unsigned long request,const void* argument,int rc,int error,bool enabled);
}
