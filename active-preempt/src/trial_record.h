#pragma once
#include "rm_control.h"
#include <ostream>
#include <stdexcept>
#include <string>
namespace ap {
inline std::string optional_ns(uint64_t n){return n?std::to_string(n):"";}
struct TrialRecord {
    unsigned trial=0;std::string mode,run_kind="performance";bool graph=false,bg_present=true,force=false,bypass=false,complete=false;
    uint64_t trigger=0,submit_begin=0,submit_end=0,ipc_send=0,ipc_received=0,ipc_ack=0;
    uint64_t entry_observed=0,main_observed=0,done_observed=0,bg_main_observed=0;
    uint64_t entry_gpu=0,main_gpu=0,done_gpu=0,bg_main_gpu=0,bg_done_gpu=0,poll_gap=0;
    uint32_t entry_node=0,main_node=0;bool bg_done_before=false,bg_done_before_control=false,overflow=false;
    int int_correct=-1,bg_correct=-1,progress_correct=-1;
    std::string failure,control_target_running="unknown";
    ControlResult control{};
    bool control_pending=false;
    static void header(std::ostream& f){
        f<<"schema_version,trial,mode,run_kind,force,bypass,graph,bg_present,trial_state,T_cpu_trigger,T_int_submit_begin,T_int_submit_end,T_ipc_send,T_ipc_received,T_ipc_ack,T_rm_call_begin,T_rm_call_end,rm_syscall_result,rm_errno,rm_status,operation_seq,T_int_graph_entry_observed,T_int_main_entry_observed,T_int_graph_done_observed,T_bg_main_observed,T_int_graph_entry_gpu_ns,T_int_main_entry_gpu_ns,T_int_graph_done_gpu_ns,T_bg_main_gpu_ns,T_bg_done_gpu_ns,int_entry_node,int_main_node,launch_id,bg_done_before_interaction,bg_done_before_control_observed,control_target_running,application_valid,control_status,gpu_overlap,ordering_relative_to_rm,correctness_status,bg_correct,int_correct,diagnostic_progress_correct,observer_max_poll_gap_ns,heartbeat_overflow,T_bg_preempted_observed,T_bg_resumed_observed,failure\n";
    }
    void write(std::ostream& f)const{
        // An incomplete BG/recovery path must not discard an already measured
        // INT application interval. Trial completion remains a separate field.
        bool app=trigger&&entry_observed>=trigger&&done_observed>=entry_observed&&entry_observed;
        std::string correctness=int_correct<0||(bg_present&&bg_correct<0)?"unknown":(int_correct==1&&(!bg_present||bg_correct==1)?"PASS":"CORRECTNESS_FAILURE");
        std::string overlap=!bg_present?"not_applicable":(!bg_main_gpu||!bg_done_gpu||!entry_gpu||!done_gpu?"unknown":(entry_gpu<bg_done_gpu&&bg_main_gpu<done_gpu?"lifetime_overlap":"no_lifetime_overlap"));
        std::string ordering=control_pending?"ordering_ambiguous":(!control.attempted?"not_applicable":(entry_observed&&entry_observed<control.begin_ns?"before_rm":"ordering_ambiguous"));
        f<<2<<','<<trial<<','<<mode<<','<<run_kind<<','<<force<<','<<bypass<<','<<graph<<','<<bg_present<<','<<(complete?"complete":"incomplete")<<',';
        for(auto n:{trigger,submit_begin,submit_end,ipc_send,ipc_received,ipc_ack,control.begin_ns,control.end_ns})f<<optional_ns(n)<<',';
        if(control.attempted)f<<control.syscall_result<<','<<control.syscall_errno<<',';else f<<",,";
        if(control.rm_status!=0xffffffff)f<<control.rm_status;f<<',';
        if(control.operation_seq)f<<control.operation_seq;f<<',';
        for(auto n:{entry_observed,main_observed,done_observed,bg_main_observed,entry_gpu,main_gpu,done_gpu,bg_main_gpu,bg_done_gpu})f<<optional_ns(n)<<',';
        if(entry_gpu)f<<entry_node;f<<',';if(main_gpu)f<<main_node;f<<','<<trial<<',';
        if(bg_present)f<<bg_done_before;f<<',';
        if(bg_present&&ipc_send)f<<bg_done_before_control;f<<','<<control_target_running<<','<<app<<','<<(control_pending?"CONTROL_RESULT_UNAVAILABLE":control.category())<<','<<overlap<<','<<ordering<<','<<correctness<<',';
        if(bg_correct>=0)f<<bg_correct;f<<',';if(int_correct>=0)f<<int_correct;f<<',';if(progress_correct>=0)f<<progress_correct;
        f<<','<<poll_gap<<',';if(bg_present)f<<overflow;f<<",,,\"";
        for(char c:failure){if(c=='"')f<<'"';f<<(c=='\n'?' ':c);}f<<"\"\n";f.flush();
        if(!f)throw std::runtime_error("Cannot persist trial evidence");
    }
};
}
