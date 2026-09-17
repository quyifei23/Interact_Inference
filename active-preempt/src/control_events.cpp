#include "control_events.h"
#include "json_log.h"
#include <ctrl/ctrla06c.h>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
namespace ap {
namespace {
struct Header {uint64_t magic=0x41504556454e5432ull;uint32_t schema=2,capacity=event_capacity,count=0,overflow=0;char owner[32]{},run_id[192]{},profile[32]{},source_commit[64]{};};
constexpr size_t event_offset=4096;
void dump(const void* data,size_t length,const std::string& path){
    const auto& h=*static_cast<const Header*>(data);
    if(h.magic!=Header{}.magic||h.schema!=2||length!=event_offset+sizeof(ControlEvent)*event_capacity)throw std::runtime_error("Invalid control journal ABI");
    std::ofstream f(path);if(!f)throw std::runtime_error("Cannot save control event log");
    const auto* events=reinterpret_cast<const ControlEvent*>(static_cast<const char*>(data)+event_offset);
    for(size_t i=0;i<std::min<size_t>(__atomic_load_n(&h.count,__ATOMIC_ACQUIRE),event_capacity);++i){
        const auto& e=events[i];uint32_t state=__atomic_load_n(&e.state,__ATOMIC_ACQUIRE);if(!state)continue;
        PreparationEvent prep{};
        const bool local=e.command==0&&e.size==sizeof(prep);
        if(local)std::memcpy(&prep,e.params,sizeof(prep));
        const bool preparation=local&&prep.magic==PreparationEvent{}.magic&&prep.revision==1;
        f<<"{\"schema_version\":2,\"run_id\":"<<json_string(h.run_id)<<",\"owner\":"<<json_string(h.owner)<<",\"pid\":"<<e.pid<<",\"trial_id\":"<<e.trial<<",\"operation_seq\":"<<e.sequence
         <<",\"driver_profile\":"<<json_string(h.profile[0]?h.profile:"550.120")<<",\"source_commit\":"<<json_string(h.source_commit)<<",\"command\":"<<(preparation?"null":std::to_string(e.command))<<",\"params_size\":"<<e.size<<",\"params_hex\":\"";
        for(unsigned j=0;j<e.size&&j<event_param_capacity;++j)f<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(e.params[j]);
        f<<std::dec<<"\",\"target_scope\":"<<json_string(e.group&&e.object==e.group?"group":"channel_or_subdevice")
         <<",\"target\":{\"hClient\":"<<e.client<<",\"hDevice\":"<<e.device<<",\"hSubdevice\":"<<e.subdevice<<",\"hTSG\":"<<e.group<<",\"hChannel\":";
        if(e.channel)f<<e.channel;else f<<"null";
        f<<",\"hObject\":"<<e.object<<",\"fd\":"<<e.fd<<",\"client_generation\":"<<e.client_generation<<",\"group_generation\":"<<e.group_generation<<",\"target_generation\":"<<e.target_generation<<"}";
        f<<",\"hardware_tsg_id\":";if(e.tsg_id!=0xffffffff||(e.command==NVA06C_CTRL_CMD_GET_INFO&&!e.channel&&state==2&&e.result.ok()))f<<e.tsg_id;else f<<"null";
        f<<",\"engine_type\":";if(e.engine)f<<e.engine;else f<<"null";
        f<<",\"event_kind\":"<<json_string(preparation?(prep.noop?"CONTROL_PREPARATION_NOOP":"CONTROL_PREPARATION"):"RM_CONTROL")
         <<",\"attempted\":"<<(state==2?(e.result.attempted?"true":"false"):"null")
         <<",\"outcome\":"<<json_string(state==2?e.result.category():"unknown");
        if(preparation){
            f<<",\"T_owner_prepare_begin\":"<<prep.timing.prepare_begin_ns<<",\"T_owner_prepare_end\":";
            if(state==2)f<<prep.timing.prepare_end_ns;else f<<"null";
            f<<",\"T_owner_action_end\":";if(state==2)f<<prep.timing.action_end_ns;else f<<"null";
        }
        f<<",\"hardware_channel_id\":null,\"runlist_id\":null,\"operation_state\":"<<json_string(state==1?"IN_FLIGHT":(preparation?"LOCAL_PREPARATION_COMPLETED":(e.result.attempted?"RETURNED":"REJECTED_BEFORE_IOCTL")))<<",\"call_begin_ns\":";
        if(!preparation&&e.result.begin_ns)f<<e.result.begin_ns;else f<<"null";
        if(state==2){f<<",\"call_end_ns\":";if(e.result.end_ns)f<<e.result.end_ns;else f<<"null";
            f<<",\"syscall_return\":";if(e.result.attempted)f<<e.result.syscall_result;else f<<"null";
            f<<",\"errno\":";if(e.result.attempted)f<<e.result.syscall_errno;else f<<"null";
            f<<",\"rm_status\":";if(e.result.rm_status!=0xffffffff)f<<e.result.rm_status;else f<<"null";
            f<<",\"rm_status_valid\":"<<(e.result.attempted&&e.result.syscall_result==0&&e.result.rm_status!=0xffffffff?"true":"false")<<",\"control_status\":"<<json_string(e.result.category());
        }else f<<",\"call_end_ns\":null,\"syscall_return\":null,\"errno\":null,\"rm_status\":null,\"control_status\":\"unknown\"";
        f<<"}\n";
    }
    if(h.overflow)f<<"{\"schema_version\":2,\"operation_state\":\"JOURNAL_FULL_NO_FURTHER_CONTROLS\"}\n";
    f.flush();if(!f)throw std::runtime_error("Cannot persist control event log; retain binary journal");
}
}
void ControlJournal::open(const std::string& path,const std::string& owner){
    if(data_)throw std::runtime_error("Journal already open");
    length_=event_offset+sizeof(ControlEvent)*event_capacity;fd_=::open(path.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    if(fd_<0||ftruncate(fd_,length_))throw std::runtime_error("Cannot create control journal");
    data_=mmap(nullptr,length_,PROT_READ|PROT_WRITE,MAP_SHARED,fd_,0);
    if(data_==MAP_FAILED){data_=nullptr;throw std::runtime_error("Cannot map control journal");}
    std::memset(data_,0,length_);auto* h=new(data_) Header{};
    std::strncpy(h->owner,owner.c_str(),sizeof(h->owner)-1);std::strncpy(h->run_id,path.substr(0,path.find_last_of('/')).c_str(),sizeof(h->run_id)-1);
    std::strncpy(h->profile,build_profile().version,sizeof(h->profile)-1);std::strncpy(h->source_commit,build_profile().source_commit,sizeof(h->source_commit)-1);
}
ControlEvent* ControlJournal::begin(const Identity* id,uint32_t object,uint32_t cmd,const void* params,uint32_t size,int64_t trial,const GroupBinding* group){
    if(!data_)return nullptr;
    auto& h=*static_cast<Header*>(data_);uint32_t n=h.count;
    if(n>=event_capacity||size>event_param_capacity){h.overflow=1;return nullptr;}
    auto* e=reinterpret_cast<ControlEvent*>(static_cast<char*>(data_)+event_offset)+n;new(e)ControlEvent{};
    e->trial=trial;e->sequence=n+1;e->pid=getpid();e->object=object;e->command=cmd;e->size=size;
    if(size&&params)std::memcpy(e->params,params,size);
    if(id){e->client=id->client;e->device=id->device;e->subdevice=id->subdevice;e->group=id->group;e->channel=id->compute_channel;e->fd=id->fd;e->tsg_id=id->tsg_id;e->engine=id->engine;
        e->client_generation=id->binding.client_generation;e->group_generation=id->binding.group.generation;
        for(auto t:{id->binding.device,id->binding.subdevice,id->binding.group,id->binding.compute_channel})if(t.handle==object)e->target_generation=t.generation;}
    if(group){
        e->client=group->client;e->device=group->device.token.handle;e->subdevice=group->subdevice.token.handle;e->group=group->group.token.handle;
        e->fd=group->fd;e->engine=group->group.engine;e->client_generation=group->client_generation;
        e->group_generation=e->target_generation=group->group.token.generation;
    }
    e->result.begin_ns=monotonic_ns();__atomic_store_n(&e->state,1,__ATOMIC_RELEASE);__atomic_store_n(&h.count,n+1,__ATOMIC_RELEASE);return e;
}
void ControlJournal::complete(ControlEvent* e,const ControlResult& r){if(e){e->result=r;__atomic_store_n(&e->state,2,__ATOMIC_RELEASE);}}
void ControlJournal::save(const std::string& path)const{if(data_)dump(data_,length_,path);}
ControlJournal::~ControlJournal(){if(data_)munmap(data_,length_);if(fd_>=0)close(fd_);}
void ControlJournal::recover(const std::string& binary,const std::string& path){
    int fd=::open(binary.c_str(),O_RDONLY|O_CLOEXEC);if(fd<0)throw std::runtime_error("Cannot read control journal");struct stat st{};
    if(fstat(fd,&st)||st.st_size<off_t(event_offset)){close(fd);throw std::runtime_error("Truncated control journal");}
    void* p=mmap(nullptr,st.st_size,PROT_READ,MAP_PRIVATE,fd,0);close(fd);if(p==MAP_FAILED)throw std::runtime_error("Cannot map recovery journal");
    try{dump(p,st.st_size,path);}catch(...){munmap(p,st.st_size);throw;}munmap(p,st.st_size);
}
}
