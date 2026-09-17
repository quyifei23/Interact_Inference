#pragma once
#include "rm_control.h"
#include <cstddef>
namespace ap {
constexpr size_t event_capacity=32768,event_param_capacity=544;
struct ControlEvent {
    uint32_t state=0; // 0 unused, 1 issued/in-flight, 2 returned/rejected
    int64_t trial=-1;
    uint64_t sequence=0,client_generation=0,group_generation=0,target_generation=0;
    uint32_t pid=0,client=0,device=0,subdevice=0,group=0,channel=0,object=0,command=0,size=0,tsg_id=0xffffffff,engine=0;
    int fd=-1;
    unsigned char params[event_param_capacity]{};
    ControlResult result{};
};
// mmap file survives controller/worker crashes. Recording uses fixed slots and
// no allocation or filesystem syscalls on the per-control path.
class ControlJournal {
public:
    void open(const std::string& path,const std::string& owner);
    ControlEvent* begin(const Identity* id,uint32_t object,uint32_t cmd,const void* params,uint32_t size,int64_t trial);
    void complete(ControlEvent* event,const ControlResult& result);
    void save(const std::string& jsonl)const;
    ~ControlJournal();
    ControlJournal()=default;
    ControlJournal(const ControlJournal&)=delete;
    static void recover(const std::string& binary,const std::string& jsonl);
private:
    int fd_=-1;void* data_=nullptr;size_t length_=0;
};
}
