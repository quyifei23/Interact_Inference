// Synthetic, CPU-only tests. No GPU scheduling requests or benchmark samples.
#include "object_registry.h"
#include "mode_plan.h"
#include "control_events.h"
#include "recovery.h"
#include "options.h"
#include "trial_record.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <unistd.h>
namespace {
void need(bool v,const char* msg){if(!v)throw std::runtime_error(msg);}
template<class F>void rejects(F f){bool caught=false;try{f();}catch(const std::exception&){caught=true;}need(caught,"expected rejection");}
void populate(ap::ObjectRegistry& r,uint32_t client=1,uint32_t group=30,uint32_t ch=40){
    r.client(client,99);r.allocate(client,10,client,0x80);r.allocate(client,20,10,0x2080);
    r.allocate(client,group,10,0xa06c);r.allocate(client,ch,group,0xc56f);r.allocate(client,ch+1,ch,0xc5c0);
}
void registry_tests(){
    ap::ObjectRegistry r;populate(r);auto old=r.discover();need(r.valid(old),"unique TSG");
    r.free(1,30);need(!r.contains(1,40)&&!r.contains(1,41),"recursive free");
    r.allocate(1,30,10,0xa06c);r.allocate(1,40,30,0xc56f);r.allocate(1,41,40,0xc5c0);
    r.allocate(1,500,10,0x9999);r.free(1,500);
    auto fresh=r.discover();need(r.valid(fresh)&&r.contains(1,41),"old tombstone polluted generation 2");
    need(!r.valid(old),"old generation accepted");
    r.bind(1,40,0);need(!r.valid(fresh),"rebind didn't invalidate identity");
    fresh=r.discover();r.allocate(1,45,30,0xc56f);need(!r.valid(fresh),"new channel not detected");
    r.allocate(1,46,45,0xc5c0);rejects([&]{r.discover();}); // same TSG, two compute channels
    ap::ObjectRegistry multi;populate(multi);multi.allocate(1,31,10,0xa06c);multi.allocate(1,50,31,0xc56f);multi.allocate(1,51,50,0xc5c0);rejects([&]{multi.discover();});
    ap::ObjectRegistry no_child;populate(no_child);no_child.free(1,41);rejects([&]{no_child.discover();});
    ap::ObjectRegistry subdevices;populate(subdevices);auto single=subdevices.discover();subdevices.allocate(1,21,10,0x2080);need(!subdevices.valid(single),"new subdevice ambiguity left cached identity valid");
    ap::ObjectRegistry clients;populate(clients,1);populate(clients,2);clients.free(1,1);need(clients.discover().client==2,"client free crossed ownership boundary");
    auto prior=clients.discover();clients.free(2,2);populate(clients,2);need(!clients.valid(prior),"client handle reuse accepted old FD generation");
    ap::ObjectRegistry overflow(2);populate(overflow);rejects([&]{overflow.discover();});
    ap::ObjectRegistry incomplete;populate(incomplete);incomplete.incomplete("synthetic capture exception");rejects([&]{incomplete.discover();});
    ap::ObjectRegistry unsupported;populate(unsupported);unsupported.incomplete("unsupported FINN allocation ABI");rejects([&]{unsupported.discover();});
    std::cout<<"registry: discovery, ambiguity, recursion, reuse, generation, client isolation, incompleteness passed\n";
}
void mode_tests(){
    for(auto name:{"none","int-only"}){auto p=ap::mode_plan(name);need(!p.identity&&p.trigger(false,false)==ap::Trigger::None,"baseline issued RM");}
    auto rt=ap::mode_plan("realtime-only");need(rt.trigger(true,true)==ap::Trigger::None,"realtime-only restarts");
    auto c=ap::mode_plan("realtime");need(c.name=="realtime-restart","alias");rejects([&]{c.trigger(true,false);});
    need(c.trigger(true,true)==ap::Trigger::Restart,"restart route");
    for(auto name:{"timeslice","preempt-wait","preempt-async","realtime-only","realtime-restart","disable","disable-split"})rejects([&]{ap::mode_plan(name).trigger(false,true);});
    need(ap::profile_matches("Kernel Module  550.120\n"),"profile rejected");
    for(auto version:{"595.58.03","550.120.1","1550.120",""})need(!ap::profile_matches(version),"unverified profile accepted");
    char exe[]="test",cuda[]="--probe-cuda",active[]="--mode",mode[]="preempt-wait";char* args[]={exe,cuda,active,mode};
    need(ap::parse(4,args).probe==ap::Probe::Cuda,"CUDA probe depended on RM/host admission");
    ap::AsyncPreemptGate async;need(async.may_issue(),"initial async state");async.submitted();need(!async.may_issue(),"repeat async allowed without completion");async.drained();need(async.may_issue(),"drained async cannot resume");
    std::cout<<"mode routing and driver profile boundary passed\n";
}
void recovery_tests(){
    struct Mock {
        bool fail_enable=false;int enabled=0,demoted=0,restored=0;
        ap::ControlResult status(bool fail=false){ap::ControlResult r;r.attempted=true;r.syscall_result=0;r.rm_status=fail?0x65:0;return r;}
        ap::ControlResult disable(bool x){need(!x,"cleanup disabled instead of enabled");++enabled;return status(fail_enable);}
        ap::ControlResult realtime(bool x){need(!x,"cleanup promoted instead of demoted");++demoted;return status();}
        ap::ControlResult timeslice(uint64_t x){need(x==5000,"incorrect original timeslice");++restored;return status();}
    } rm;
    ap::Recovery r;r.disabled=r.realtime=r.timeslice=true;r.original_timeslice=5000;std::ostringstream s;
    need(r.restore(rm,s)&&rm.enabled==1&&rm.demoted==1&&rm.restored==1,"failed control couldn't restore");
    r.disabled=true;rm.fail_enable=true;need(!r.restore(rm,s)&&r.disabled,"enable failure erased recovery obligation");
    std::cout<<"owner recovery flags and enable failure retention passed\n";
}
void event_tests(){
    char path[]="/tmp/ap-synthetic-events-XXXXXX";need(mkdtemp(path)!=nullptr,"mkdtemp");
    std::filesystem::path dir(path);auto binary=(dir/"control.bin").string();
    {
        ap::ControlJournal j;j.open(binary,"synthetic-offline");uint32_t p=1;
        auto* success=j.begin(nullptr,30,0xa06c0105,&p,sizeof(p),0);
        ap::ControlResult ok;ok.attempted=true;ok.syscall_result=0;ok.rm_status=0;ok.begin_ns=10;ok.end_ns=20;j.complete(success,ok);
        // Returned PREEMPT followed by CUDA timeout: the record must survive
        // without a complete trial. Failed disable and failed enable are separate.
        auto* failed=j.begin(nullptr,20,0x2080110b,&p,sizeof(p),1);auto bad=ok;bad.rm_status=0x65;j.complete(failed,bad);
        auto* restored=j.begin(nullptr,20,0x2080110b,&p,sizeof(p),1);j.complete(restored,ok);
        auto* enable_failed=j.begin(nullptr,20,0x2080110b,&p,sizeof(p),2);j.complete(enable_failed,bad);
        j.begin(nullptr,30,0xa06c0105,&p,sizeof(p),3); // controller killed mid-ioctl
        j.save((dir/"normal.jsonl").string());
        if(std::filesystem::exists("/dev/full"))rejects([&]{j.save("/dev/full");});
    }
    ap::ControlJournal::recover(binary,(dir/"recovered.jsonl").string());
    std::ifstream f(dir/"recovered.jsonl");std::string text((std::istreambuf_iterator<char>(f)),{});
    need(text.find("CONTROL_ACCEPTED_EFFECT_UNVERIFIED")!=std::string::npos,"accepted record lost");
    need(text.find("CONTROL_TIMEOUT")!=std::string::npos,"failed record lost");
    need(text.find("IN_FLIGHT")!=std::string::npos&&text.find("\"syscall_return\":null")!=std::string::npos,"inflight fabricated success");
    size_t lines=0;for(char c:text)lines+=c=='\n';need(lines==5,"independent events lost");
    std::filesystem::remove_all(dir);std::cout<<"control event crash recovery / incomplete trial / restore outcomes passed\n";
}
}
int main(int argc,char** argv){try{
    if(argc==3&&std::string(argv[1])=="--emit-synthetic-fixtures"){
        std::filesystem::path dir(argv[2]);std::ofstream csv(dir/"raw.csv");ap::TrialRecord::header(csv);
        ap::TrialRecord r;r.mode="realtime-restart";r.run_kind="diagnostic";r.graph=true;r.trigger=1000;r.entry_observed=2000;r.main_observed=7000;r.done_observed=8000;
        r.entry_gpu=10000;r.main_gpu=14000;r.done_gpu=15000;r.bg_main_gpu=5000;r.bg_done_gpu=20000;r.int_correct=r.bg_correct=1;r.complete=true;
        r.control.attempted=true;r.control.begin_ns=3000;r.control.end_ns=4000;r.control.syscall_result=0;r.control.rm_status=0;r.write(csv);
        r.trial=1;r.complete=false;r.bg_correct=-1;r.failure="BG timeout, known INT interval and RM result retained";r.write(csv);
        ap::TrialRecord solo;solo.mode="int-only";solo.run_kind="diagnostic";solo.bg_present=false;solo.write(csv);
        r.trial=3;r.done_observed=r.done_gpu=0;r.int_correct=-1;r.failure="INT timeout, known RM result retained";r.write(csv);
        ap::ControlJournal j;j.open((dir/"events.bin").string(),"synthetic-offline");uint32_t p=1;auto* e=j.begin(nullptr,30,0xa06c0105,&p,sizeof(p),0);j.complete(e,r.control);j.begin(nullptr,30,0xa06c0105,&p,sizeof(p),1);j.save((dir/"events.jsonl").string());
        return 0;
    }
    registry_tests();mode_tests();recovery_tests();event_tests();return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
