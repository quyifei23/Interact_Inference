// CPU-only synthetic fixtures. No real FD, CUDA workload or GPU measurement.
#include "group_query_internal.h"
#include "options.h"
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
#include <stdexcept>
#include <type_traits>
namespace {
void need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F f){bool caught=false;try{f();}catch(const std::exception&){caught=true;}need(caught,"expected rejection");}
void group(ap::ObjectRegistry& r,unsigned channels=1,uint32_t handle=30,bool compute=true){
    r.allocate(1,handle,10,0xa06c,compute?1:11);
    for(unsigned i=0;i<channels;++i){uint32_t ch=handle*100+i*3;
        r.allocate(1,ch,handle,0xc56f);
        r.allocate(1,ch+1,ch,compute?0xc6c0:0xc6b5);
        if(compute)r.allocate(1,ch+2,ch,0xc6b5);
    }
}
void populate(ap::ObjectRegistry& r,unsigned channels=1){r.client(1,99);r.allocate(1,10,1,0x80);r.allocate(1,20,10,0x2080);group(r,channels);}
void lifecycle(){
    for(unsigned channels:{1u,2u,3u,8u,17u}){
        ap::ObjectRegistry r;populate(r,channels);auto b=r.discover_group();
        need(r.valid_group(b)&&b.members.size()==channels,"complete group membership or legal channel count");
        for(const auto& m:b.members)need(m.compute_children.size()==1&&m.channel.engine==0,"compute children lost or engine invented");
        if(channels==1)need(r.valid(r.discover()),"strict single-channel binding regressed");else rejects([&]{r.discover();});
    }
    ap::ObjectRegistry r;populate(r,8);for(uint32_t h:{31u,32u,33u})group(r,4,h,false);
    auto b=r.discover_group();need(b.members.size()==8&&b.group.token.handle==30,"copy-only TSGs counted as compute");
    r.allocate(1,6000,10,0x9999);r.free(1,6000);need(r.valid_group(b),"unrelated FREE poisoned binding");
    r.free(1,3100);need(r.valid_group(b),"copy-only group member FREE poisoned binding");
    r.client(2,100);r.allocate(2,10,2,0x80);r.free(2,2);need(r.valid_group(b),"unrelated client FREE poisoned binding");
    group(r,1,34);need(!r.valid_group(b),"new compute TSG did not invalidate");rejects([&]{r.discover_group();});
    r.free(1,34);need(!r.valid_group(b),"transient second compute candidate revived old binding");b=r.discover_group();
    r.allocate(1,9000,30,0xc56f);need(!r.valid_group(b),"new channel ignored");r.free(1,9000);need(!r.valid_group(b),"transient membership change ignored");
    b=r.discover_group();r.free(1,3000);need(!r.valid_group(b),"removed channel ignored");
    r.allocate(1,3000,30,0xc56f);r.allocate(1,3001,3000,0xc6c0);need(!r.valid_group(b),"reused channel revived snapshot");
    b=r.discover_group();r.bind(1,3000,1);need(!r.valid_group(b),"channel rebind ignored");
    b=r.discover_group();r.allocate(1,3001,3003,0xc6c0);need(!r.valid_group(b),"compute ancestry change ignored");
    b=r.discover_group();r.allocate(1,9800,3003,0xc6c0);r.free(1,9800);need(!r.valid_group(b),"transient compute relation ignored");
    b=r.discover_group();r.free(1,30);group(r,8);need(!r.valid_group(b),"group reuse accepted");
    b=r.discover_group();r.free(1,20);r.allocate(1,20,10,0x2080);need(!r.valid_group(b),"subdevice reuse accepted");
    b=r.discover_group();r.free(1,10);r.allocate(1,10,1,0x80);r.allocate(1,20,10,0x2080);group(r,8);need(!r.valid_group(b),"device reuse accepted");
    b=r.discover_group();r.free(1,1);populate(r,8);need(!r.valid_group(b),"client reuse accepted");
    for(const auto& reason:{"capture exception","unsupported FINN ABI","capacity overflow"}){
        ap::ObjectRegistry bad;populate(bad);auto old=bad.discover_group();bad.incomplete(reason);need(!bad.valid_group(old),"incomplete cached binding accepted");rejects([&]{bad.discover_group();});
    }
    ap::ObjectRegistry overflow(2);populate(overflow);rejects([&]{overflow.discover_group();});
    ap::ObjectRegistry no_compute;populate(no_compute);no_compute.free(1,3001);rejects([&]{no_compute.discover_group();});
    ap::ObjectRegistry orphan;populate(orphan);orphan.allocate(1,3001,10,0xc6c0);rejects([&]{orphan.discover_group();});
    ap::ObjectRegistry parent;populate(parent);parent.allocate(1,9001,9999,0xc6c0);rejects([&]{parent.discover_group();});
    ap::ObjectRegistry bad_device;bad_device.client(1,99);bad_device.allocate(1,10,9999,0x80);bad_device.allocate(1,20,10,0x2080);group(bad_device);rejects([&]{bad_device.discover_group();});
    ap::ObjectRegistry subs;populate(subs);auto single=subs.discover_group();subs.allocate(1,21,10,0x2080);need(!subs.valid_group(single),"subdevice ambiguity ignored");
    ap::ObjectRegistry source,destination;populate(source);populate(destination);auto historical=source.discover_group();
    need(!destination.valid_group(historical),"same numeric synthetic handles from different registry accepted");
    historical.owner_pid^=1;need(!source.valid_group(historical),"old process identity accepted");
    pid_t child=fork();need(child>=0,"fork failed");if(!child){bool rejected=false;try{source.discover_group();}catch(const std::runtime_error&){rejected=true;}_exit(rejected?0:2);}
    int status=0;waitpid(child,&status,0);need(WIFEXITED(status)&&WEXITSTATUS(status)==0,"fork inherited operational group");
    std::cout<<"group registry: legal sizes, strict path, copy-only exclusion, topology revisions, reuse, ancestry, incompleteness and process scope passed\n";
}
struct Mock {
    const ap::GroupBinding* b;unsigned calls=0;int rc=0,error=0;uint32_t status=NV_OK,tsg=0;
    static int invoke(int fd,unsigned long request,void* ptr,void* context){
        auto& m=*static_cast<Mock*>(context);auto& a=*static_cast<NVOS54_PARAMETERS*>(ptr);++m.calls;
        need(fd==m.b->fd&&request==_IOWR('F',NV_ESC_RM_CONTROL,NVOS54_PARAMETERS),"incorrect FD/escape");
        need(a.hClient==m.b->client&&a.hObject==m.b->group.token.handle&&a.cmd==NVA06C_CTRL_CMD_GET_INFO,"wrong target or non-GET_INFO command");
        need(a.flags==0&&a.paramsSize==sizeof(NVA06C_CTRL_GET_INFO_PARAMS)&&a.status==0xffffffff,"uninitialized control envelope");
        auto& params=*static_cast<NVA06C_CTRL_GET_INFO_PARAMS*>(a.params);need(params.tsgID==0,"uninitialized GET_INFO input");
        params.tsgID=m.tsg;a.status=m.status;errno=m.error;return m.rc;
    }
};
void query_tests(){
    static_assert(!std::is_convertible_v<ap::GroupBinding,ap::Binding>);
    static_assert(!std::is_convertible_v<ap::GroupIdentity,ap::Identity>);
    static_assert(!std::is_constructible_v<ap::RmControl,ap::GroupIdentity>);
    static_assert(!std::is_default_constructible_v<ap::GroupIdentity>);
    const auto& profile=ap::build_profile();ap::ObjectRegistry r(65536,profile.version,profile.source_commit);populate(r,8);auto b=r.discover_group();
    char dir[]="/tmp/ap-group-synthetic-XXXXXX";need(mkdtemp(dir),"mkdtemp");
    {
        ap::ControlJournal journal;journal.open(std::string(dir)+"/events.bin","synthetic-offline-group");
        auto state=[&](ap::RmStage stage){ap::ProfileState s;s.configure(profile,profile.version,stage,false);s.group_binding_observed=s.cuda_minimal_workload_passed=s.single_gpu_scope_verified=true;return s;};
        ap::detail::GroupInfoOnce query;auto s=state(ap::RmStage::GroupInfo);Mock mock{&b};
        auto got=query.query(r,s,b,true,journal,-1,Mock::invoke,&mock);
        need(got.control.ok()&&got.hardware_tsg_id&&*got.hardware_tsg_id==0,"TSG ID zero rejected");
        need(s.group_get_info_verified&&!s.binding_observed&&!s.readonly_verified&&!s.may_readonly()&&!s.may_active()&&!s.active_experiment_authorized&&!s.active_result_measured,"group success unlocked channel/active");
        need(s.project_controls_attempted==1&&s.project_readonly_controls_attempted==1&&s.project_active_controls_attempted==0,"control counts mixed");
        need(!query.query(r,s,b,true,journal,-1,Mock::invoke,&mock).control.attempted&&mock.calls==1,"query repeated");
        for(auto stage:{ap::RmStage::Observe,ap::RmStage::Readonly,ap::RmStage::Active,ap::RmStage::Disabled}){
            auto no=state(stage);ap::detail::GroupInfoOnce guard;
            need(!guard.query(r,no,b,true,journal,-1,Mock::invoke,&mock).control.attempted&&!no.project_controls_attempted,"incorrect stage appended project control");
        }
        for(int fail=0;fail<3;++fail){
            auto fs=state(ap::RmStage::GroupInfo);ap::detail::GroupInfoOnce q;Mock m{&b};
            if(fail==0){m.rc=-1;m.error=EBADF;m.status=NV_OK;} // even if status was overwritten, syscall failed
            if(fail==1)m.status=NV_ERR_INVALID_OBJECT;
            if(fail==2)m.status=0xffffffff;
            auto failed=q.query(r,fs,b,true,journal,-1,Mock::invoke,&m);
            need(!failed.control.ok()&&!failed.hardware_tsg_id&&!fs.group_get_info_verified,"failure became verified");
            need(failed.control.syscall_result==m.rc&&failed.control.syscall_errno==m.error&&failed.control.rm_status==m.status,"raw failure lost");
            need(!q.query(r,fs,b,true,journal,-1,Mock::invoke,&m).control.attempted&&m.calls==1,"failed query retried");
        }
        for(int gate=0;gate<8;++gate){
            auto bs=state(ap::RmStage::GroupInfo);auto bad=b;ap::detail::GroupInfoOnce q;Mock m{&bad};bool fd=true;
            if(gate==0)bs.runtime_version="unreviewed";
            if(gate==1)bad.source_commit="historical-profile";
            if(gate==2)bad.owner_pid^=1;
            if(gate==3)bad.members.pop_back();
            if(gate==4)bs.single_gpu_scope_verified=false;
            if(gate==5)bs.cuda_minimal_workload_passed=false;
            if(gate==6)fd=false;
            if(gate==7)bad.fd=88;
            need(!q.query(r,bs,bad,fd,journal,-1,Mock::invoke,&m).control.attempted&&!m.calls,"identity/profile/scope/FD gate bypassed");
        }
        auto bs=state(ap::RmStage::GroupInfo);r.bind(1,3000,11);auto wrong_engine=r.discover_group();ap::detail::GroupInfoOnce q;Mock m{&wrong_engine};
        need(!q.query(r,bs,wrong_engine,true,journal,-1,Mock::invoke,&m).control.attempted&&!m.calls,"non-GR compute engine accepted");
        journal.save(std::string(dir)+"/events.jsonl");
        std::ifstream f(std::string(dir)+"/events.jsonl");std::string lines((std::istreambuf_iterator<char>(f)),{});
        need(lines.find("\"hardware_tsg_id\":0")!=std::string::npos&&lines.find("\"hChannel\":null")!=std::string::npos,"group journal lost zero ID or invented channel");
        need(lines.find("\"syscall_return\":-1,\"errno\":9,\"rm_status\":0")!=std::string::npos,"failed syscall/raw status missing from journal");
    }
    std::filesystem::remove_all(dir);
    std::cout<<"group GET_INFO: one-shot exact target/envelope, independent counters, failure journaling, no channel/active promotion passed\n";
}
void cli_tests(){
    char exe[]="test",flag[]="--probe-rm-group-info";char* args[]={exe,flag};
    need(ap::parse(2,args).probe==ap::Probe::GroupInfo,"group CLI missing");
    char active[]="--test-host-confirmed";char* bad[]={exe,flag,active};rejects([&]{ap::parse(3,bad);});
    char readonly[]="--probe-rm-readonly";char* mixed[]={exe,flag,readonly};rejects([&]{ap::parse(3,mixed);});
    need(ap::group_probe_visibility_matches("GPU-00112233-4455-6677-8899-aabbccddeeff","00112233445566778899aabbccddeeff",1),"explicit UUID selection");
    for(const auto& visible:{"","0","GPU-00112233","GPU-00112233-4455-6677-8899-aabbccddeeff,1"})need(!ap::group_probe_visibility_matches(visible,"00112233445566778899aabbccddeeff",1),"ambiguous GPU selection accepted");
    need(!ap::group_probe_visibility_matches("GPU-00112233-4455-6677-8899-aabbccddeeff","00112233445566778899aabbccddeeff",2),"multi-GPU selection accepted");
}
}
int main(){try{lifecycle();query_tests();cli_tests();std::cout<<"All group fixtures synthetic; GPU trials=0\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
