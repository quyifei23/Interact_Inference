// Synthetic CPU-only fixtures: transports below never issue an ioctl.
#include "group_query_internal.h"
#include "options.h"
#include "protocol.h"
#include "trial_record.h"
#include <nvos.h>
#include <nv_escape.h>
#include <nvstatus.h>
#include <ctrl/ctrla06c.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace {
void need(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
template<class F>void rejects(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}need(rejected,"expected rejection");}
void populate(ap::ObjectRegistry& r,unsigned channels=8,uint32_t group=30){
    if(!r.has_client(1)){r.client(1,99);r.allocate(1,10,1,0x80);r.allocate(1,20,10,0x2080);}
    r.allocate(1,group,10,0xa06c,1);
    for(unsigned n=0;n<channels;++n){auto channel=group*100+n*2;r.allocate(1,channel,group,0xc56f);r.allocate(1,channel+1,channel,0xc6c0);}
}
struct Mock {
    const ap::GroupBinding* expected=nullptr;
    unsigned gets=0,preempts=0;int rc=0,error=0;uint32_t status=NV_OK,tsg=0;
    uint32_t executing_pid=0;
    static int invoke(int fd,unsigned long request,void* parameters,void* context){
        auto& m=*static_cast<Mock*>(context);const auto& b=*m.expected;auto& a=*static_cast<NVOS54_PARAMETERS*>(parameters);
        m.executing_pid=getpid();
        need(fd==b.fd&&request==_IOWR('F',NV_ESC_RM_CONTROL,NVOS54_PARAMETERS),"wrong allocating FD/escape");
        need(a.hClient==b.client&&a.hObject==b.group.token.handle&&a.hObject!=m.tsg,"target is not owned RM group handle");
        need(a.flags==0&&a.status==0xffffffff,"control envelope not fully initialized");
        if(a.cmd==NVA06C_CTRL_CMD_GET_INFO){
            ++m.gets;need(a.paramsSize==sizeof(NVA06C_CTRL_GET_INFO_PARAMS),"GET_INFO size");
            static_cast<NVA06C_CTRL_GET_INFO_PARAMS*>(a.params)->tsgID=m.tsg;a.status=NV_OK;return 0;
        }
        need(a.cmd==NVA06C_CTRL_CMD_PREEMPT,"unexpected getter or scheduling control");++m.preempts;
        need(a.paramsSize==sizeof(NVA06C_CTRL_PREEMPT_PARAMS),"PREEMPT ABI size");
        auto& p=*static_cast<NVA06C_CTRL_PREEMPT_PARAMS*>(a.params);auto* bytes=static_cast<unsigned char*>(a.params);
        need(p.bWait==NV_TRUE&&p.bManualTimeout==NV_TRUE&&p.timeoutUs>0&&p.timeoutUs<=NVA06C_CTRL_CMD_PREEMPT_MAX_MANUAL_TIMEOUT_US,"not bounded synchronous PREEMPT");
        need(bytes[2]==0&&bytes[3]==0,"PREEMPT padding uninitialized");
        a.status=m.status;errno=m.error;return m.rc;
    }
};
struct Fixture {
    const ap::DriverProfile& profile=ap::build_profile();
    ap::ObjectRegistry registry{65536,profile.version,profile.source_commit};
    ap::GroupBinding binding;
    ap::ProfileState state;ap::ControlJournal journal;
    ap::detail::GroupInfoOnce query;ap::detail::GroupPreemptOnce preempt;Mock mock;
    std::string dir;
    Fixture(unsigned channels=8,ap::RmStage stage=ap::RmStage::GroupActive,bool authorized=true){
        char tmp[]="/tmp/ap-synthetic-preempt-XXXXXX";need(mkdtemp(tmp),"mkdtemp");dir=tmp;
        populate(registry,channels);binding=registry.discover_group();mock.expected=&binding;
        state.configure(profile,profile.version,stage,authorized);
        state.cuda_minimal_workload_passed=state.group_binding_observed=state.single_gpu_scope_verified=state.workload_gpu_reviewed=true;
        state.scope_gpu_uuid="00112233445566778899aabbccddeeff";
        state.scope_visible_devices="GPU-00112233-4455-6677-8899-aabbccddeeff";
        state.authorized_visible_devices=state.scope_visible_devices;state.group_owner=ap::GroupOwner::Background;
        journal.open(dir+"/events.bin","synthetic-BG");
    }
    ~Fixture(){std::filesystem::remove_all(dir);}
    void verify(){auto r=query.query(registry,state,binding,true,journal,-1,Mock::invoke,&mock);need(r.control.ok()&&r.hardware_tsg_id==mock.tsg,"GET_INFO mock failed");}
    ap::ControlResult issue(uint32_t timeout=NVA06C_CTRL_CMD_PREEMPT_MAX_MANUAL_TIMEOUT_US,bool fd=true,int64_t trial=0,bool current=true){
        return preempt.preempt(registry,state,binding,query,fd,journal,trial,timeout,Mock::invoke,&mock,current);
    }
};
void successful_path(){
    static_assert(!std::is_convertible_v<ap::GroupIdentity,ap::Identity>);
    static_assert(!std::is_constructible_v<ap::RmControl,ap::GroupIdentity>);
    for(unsigned channels:{1u,3u,8u,13u}){
        Fixture f(channels);f.verify();
        if(channels==1)need(f.registry.valid(f.registry.discover()),"strict single-channel regression");else rejects([&]{f.registry.discover();});
        need(!f.state.binding_observed&&!f.state.readonly_verified&&!f.state.may_active(),"group GET_INFO unlocked strict/channel path");
        auto r=f.issue();need(r.ok()&&f.mock.gets==1&&f.mock.preempts==1,"group PREEMPT failed or repeated GET_INFO");
        need(f.state.project_group_get_info_attempted==1&&f.state.project_group_preempt_attempted==1&&f.state.project_active_controls_attempted==1&&f.state.project_readonly_controls_attempted==1,"counters confused");
        need(!f.state.active_result_measured,"NV_OK became observed GPU effect");
        need(!f.issue().attempted&&!f.issue(1000000,true,1).attempted&&f.mock.preempts==1,"repeated active request allowed");
    }
    Fixture f;f.verify();f.registry.allocate(1,9000,10,0x9999);f.registry.free(1,9000);
    need(f.issue().ok(),"unrelated FREE poisoned verified group");
}
void gates(){
    using R=ap::ControlResult::Rejection;
    {Fixture f(8,ap::RmStage::GroupInfo);f.verify();need(f.issue().rejection==R::Stage&&!f.mock.preempts,"readonly stage modified scheduling");}
    {Fixture f(8,ap::RmStage::GroupActive,false);f.verify();need(f.issue().rejection==R::Authorization&&!f.mock.preempts,"query success authorized active");}
    {Fixture f;need(f.issue().rejection==R::Readonly&&!f.mock.preempts,"missing exact GET_INFO accepted");}
    {Fixture f;f.verify();f.state.group_owner=ap::GroupOwner::Interactive;need(f.issue().rejection==R::Owner&&!f.mock.preempts,"INT owner issued BG control");}
    {Fixture f;f.verify();need(f.issue(0).rejection==R::TimeoutRange&&f.issue(NVA06C_CTRL_CMD_PREEMPT_MAX_MANUAL_TIMEOUT_US+1).rejection==R::TimeoutRange&&!f.mock.preempts,"invalid timeout issued syscall");}
    {Fixture f;f.verify();need(f.issue(1000000,true,1).rejection==R::Stage&&!f.mock.preempts,"second trial allowed");}
    {Fixture f;f.verify();need(f.issue(1000000,false).rejection==R::Device&&!f.mock.preempts,"closed FD accepted");}
    {Fixture f;f.verify();need(f.issue(1000000,true,0,false).rejection==R::Binding&&!f.mock.preempts,"environment mismatch misclassified or accepted");}
    for(unsigned gate=0;gate<13;++gate){
        Fixture f;f.verify();
        if(gate==0)f.binding.owner_pid^=1;
        if(gate==1)f.binding.profile_version="unknown";
        if(gate==2)f.state.runtime_version="different";
        if(gate==3)f.binding.fd=88;
        if(gate==4){f.registry.free(1,30);populate(f.registry);}
        if(gate==5){f.registry.free(1,3000);f.registry.allocate(1,3000,30,0xc56f);f.registry.allocate(1,3001,3000,0xc6c0);}
        if(gate==6)f.registry.bind(1,3000,1);
        if(gate==7)f.registry.incomplete("synthetic unsupported allocation ABI");
        if(gate==8)populate(f.registry,1,40);
        if(gate==9)f.state.scope_gpu_uuid="different-current-GPU";
        if(gate==10)f.state.authorized_visible_devices="GPU-different-authorization-scope";
        if(gate==11)f.registry.free(1,1);
        if(gate==12)f.registry.free(1,10);
        need(!f.issue().attempted&&!f.mock.preempts,"stale/foreign/incomplete/ambiguous/unscoped identity issued control");
    }
    {Fixture f;f.verify();f.registry.free(1,30);populate(f.registry);f.binding=f.registry.discover_group();
     need(f.registry.valid_group(f.binding)&&f.state.group_get_info_verified,"new current candidate setup");
     need(f.issue().rejection==R::Readonly&&!f.mock.preempts,"historical GET_INFO bool validated new generation");}
    {Fixture old,current;old.verify();current.state.group_get_info_verified=true;
     auto r=current.preempt.preempt(current.registry,current.state,current.binding,old.query,true,current.journal,0,1000000,Mock::invoke,&current.mock);
     need(r.rejection==R::Readonly&&!current.mock.preempts,"another registry's GET_INFO validated same numeric handles");}
    {Fixture f(8,ap::RmStage::Observe,false);auto r=f.query.query(f.registry,f.state,f.binding,true,f.journal,-1,Mock::invoke,&f.mock);
     need(!r.control.attempted&&!f.issue().attempted&&!f.state.project_controls_attempted,"observe-only issued controls");}
}
void failures_and_timeout_journal(){
    for(int kind=0;kind<4;++kind){
        Fixture f;f.verify();
        if(kind==0){f.mock.rc=-1;f.mock.error=EBADF;f.mock.status=NV_OK;}
        if(kind==1)f.mock.status=NV_ERR_INSUFFICIENT_PERMISSIONS;
        if(kind==2)f.mock.status=NV_ERR_TIMEOUT;
        auto r=f.issue();need(r.attempted&&r.syscall_result==f.mock.rc&&r.syscall_errno==f.mock.error&&r.rm_status==f.mock.status,"raw syscall/status evidence lost");
        need(r.ok()==(kind==3),"syscall failure with NV_OK became accepted");
        need(!f.issue().attempted&&f.mock.preempts==1,"failed syscall retried");
        // A CUDA timeout/owner crash can happen before journal.save(). Recover
        // the already-returned PREEMPT independently of the incomplete row.
        ap::TrialRecord row;row.mode="group-preempt-wait";row.control=r;row.failure="synthetic CUDA completion timeout";
        std::ostringstream csv;ap::TrialRecord::header(csv);row.write(csv);need(csv.str().find("incomplete")!=std::string::npos,"timeout became completed trial");
        ap::ControlJournal::recover(f.dir+"/events.bin",f.dir+"/recovered.jsonl");
        std::ifstream file(f.dir+"/recovered.jsonl");std::string text((std::istreambuf_iterator<char>(file)),{});
        need(text.find("\"hChannel\":null")!=std::string::npos&&text.find("\"hardware_tsg_id\":0")!=std::string::npos,"invented channel or dropped TSG zero");
        std::istringstream lines(text);std::string line,returned;
        while(std::getline(lines,line))if(line.find("\"command\":"+std::to_string(NVA06C_CTRL_CMD_PREEMPT)+",")!=std::string::npos){returned=line;break;}
        need(returned.find("\"operation_state\":\"RETURNED\"")!=std::string::npos&&returned.find("\"control_status\":\""+std::string(r.category())+"\"")!=std::string::npos,"returned PREEMPT lost after CUDA timeout (GET_INFO is not substitute evidence)");
        if(kind==0)need(returned.find("\"syscall_return\":-1,\"errno\":9,\"rm_status\":0")!=std::string::npos,"EBADF/raw NV_OK not preserved");
    }
}
void routing(){
    for(const auto& mode:{"none","int-only"}){auto p=ap::mode_plan(mode);need(!p.identity&&!p.group_identity&&p.trigger(false,false)==ap::Trigger::None,"baseline requires identity/control");}
    auto p=ap::mode_plan("group-preempt-wait");need(p.group_identity&&!p.identity&&!p.realtime&&!p.timeslice&&p.trigger(true,false)==ap::Trigger::GroupPreemptWait,"group path routed through channel configuration");
    rejects([&]{p.trigger(false,false);});
    std::vector<std::string> argv={"test","--mode","group-preempt-wait","--test-host-confirmed","--bg-iterations","8192","--int-iterations","256"};
    auto parse=[&](){std::vector<char*> args;for(auto& a:argv)args.push_back(a.data());return ap::parse(args.size(),args.data());};
    need(parse().mode==p.name,"group CLI rejected valid fixed smoke");
    for(const auto& flag:{"--graph","--diagnostic-progress","--allow-extended"}){argv.push_back(flag);rejects(parse);argv.pop_back();}
    argv.insert(argv.end(),{"--trials","2"});rejects(parse);argv.resize(argv.size()-2);
    argv.erase(argv.begin()+3);rejects(parse);
    need(ap::distinct_group_scope(10,20,"uuid","uuid",1,1,0,7),"zero TSG ID not accepted for pair");
    need(!ap::distinct_group_scope(10,20,"uuid","uuid",1,1,7,7)&&!ap::distinct_group_scope(10,20,"uuid","other",1,1,0,7)&&!ap::distinct_group_scope(10,20,"uuid","uuid",0,0,0,7),"unresolved pair scope accepted");
}
void owner_process(){
    int commands[2],results[2];need(pipe(commands)==0&&pipe(results)==0,"pipe");
    pid_t child=fork();need(child>=0,"fork");
    if(!child){
        alarm(10);close(commands[1]);close(results[0]);int code=0;
        try{
            Fixture f;f.verify();ap::Command command;
            need(read(commands[0],&command,sizeof(command))==sizeof(command)&&command==ap::Command::GroupPreemptWait,"owner command missing");
            auto r=f.issue();uint32_t response[]={uint32_t(getpid()),f.mock.executing_pid,f.mock.preempts,uint32_t(r.ok())};
            need(write(results[1],response,sizeof(response))==sizeof(response),"owner response write");
        }catch(...){code=1;}
        close(commands[0]);close(results[1]);_exit(code);
    }
    close(commands[0]);close(results[1]);auto cmd=ap::Command::GroupPreemptWait;
    need(write(commands[1],&cmd,sizeof(cmd))==sizeof(cmd),"controller command write");close(commands[1]);
    uint32_t response[4]{};auto count=read(results[0],response,sizeof(response));close(results[0]);int status=0;waitpid(child,&status,0);
    need(WIFEXITED(status)&&WEXITSTATUS(status)==0&&count==sizeof(response),"BG mock process failed");
    need(response[0]==uint32_t(child)&&response[1]==uint32_t(child)&&response[1]!=uint32_t(getpid())&&response[2]==1&&response[3]==1,"controller borrowed BG binding or multiple controls issued");
}
}
int main(){try{successful_path();gates();failures_and_timeout_journal();routing();owner_process();
    std::cout<<"Synthetic group PREEMPT: exact GET_INFO binding, multichannel, ownership/stage/auth/scope/generation gates, bounded wait, one-shot, raw failures, timeout journal and owner-process routing passed. GPU controls/trials=0.\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
