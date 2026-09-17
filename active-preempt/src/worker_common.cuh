#pragma once
#include "protocol.h"
#include "options.h"
#include "json_log.h"
#include "diagnostic_trace.h"
#include <cuda.h>
#include <cuda_runtime.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

namespace ap {
inline void cuda_check(cudaError_t e,const char* op){if(e!=cudaSuccess)throw std::runtime_error(std::string(op)+": code="+std::to_string(int(e))+" "+cudaGetErrorName(e)+" "+cudaGetErrorString(e));}
inline void cu_check(CUresult e,const char* op){if(e!=CUDA_SUCCESS){const char* msg=nullptr;cuGetErrorString(e,&msg);throw std::runtime_error(std::string(op)+": code="+std::to_string(int(e))+" "+(msg?msg:"CUDA driver error"));}}
#define CUDA_OK(x) ::ap::cuda_check((x),#x)
#define CU_OK(x) ::ap::cu_check((x),#x)
__device__ inline uint64_t global_ns(){uint64_t t;asm volatile("mov.u64 %0, %%globaltimer;":"=l"(t));return t;}
__device__ inline void publish(uint32_t* p,uint32_t v){__threadfence_system();asm volatile("st.release.sys.global.u32 [%0], %1;"::"l"(p),"r"(v):"memory");}
__device__ inline uint32_t device_acquire(uint32_t* p){uint32_t v;asm volatile("ld.acquire.sys.global.u32 %0, [%1];":"=r"(v):"l"(p):"memory");return v;}

// Finite integer arithmetic with live registers and shared-memory dependencies.
// No sleep, spin-wait, input-dependent unbounded loop, or cancellation polling.
// Fixed iterations: paused wall time never counts as completed computation.
static __global__ void arithmetic(uint32_t* output,uint64_t iterations,Telemetry* t,
                                  uint64_t heartbeat_ns,bool measured,bool entry,uint32_t node,uint32_t seed,unsigned long long* progress) {
    extern __shared__ uint32_t memory[];
    uint32_t lane=threadIdx.x,idx=blockIdx.x*blockDim.x+lane;
    uint32_t x=idx*747796405u+seed,y=idx^0x9e3779b9u;
    uint64_t begin=global_ns(),last=begin;
    uint32_t samples=0;
    if(lane==0&&measured)t->cta_start_gpu_ns[blockIdx.x]=begin;
    if(idx==0&&entry){t->entry_gpu_ns=begin;t->entry_node=node;publish(&t->entry_started,1);}
    if(idx==0 && measured){t->start_gpu_ns=begin;t->main_node=node;publish(&t->started,1);}
    for(uint64_t round=0;round<iterations;round+=256) {
        #pragma unroll 1
        for(unsigned k=0;k<256;++k){x=(x*1664525u+1013904223u)^y;y=(y<<7)|(y>>25);y+=x^0xa511e9b3u;}
        memory[lane]=x;
        __syncthreads();
        x^=memory[(lane+1)%blockDim.x];
        __syncthreads();
        if(lane==0){
            uint64_t now=global_ns();
            if(progress){atomicAdd(progress+2*blockIdx.x,1ull);atomicAdd(progress+2*blockIdx.x+1,round/256+1ull);}
            if(idx==0 && measured && heartbeat_ns && now-last>=heartbeat_ns){
                if(samples<max_samples){t->heartbeat_gpu_ns[samples++]=now;publish(&t->heartbeat_count,samples);}
                else t->overflow=1;
                last=now;
            }
        }
        __syncthreads();
    }
    output[idx]=x^y;
    if(lane==0 && measured)t->cta_end_gpu_ns[blockIdx.x]=global_ns();
}
static __global__ void finish_marker(Telemetry* t){t->done_gpu_ns=global_ns();publish(&t->done,1);}
static __global__ void ping_calibration(Telemetry* t,uint32_t count){
    const uint64_t deadline=global_ns()+gpu_watchdog_ns;
    for(uint32_t i=1;i<=count;++i){
        while(device_acquire(&t->ping_request)!=i){if(global_ns()>deadline)return;}
        t->ping_gpu_ns=global_ns();publish(&t->ping_ack,i);
    }
}

inline std::string uuid_string(const cudaUUID_t& u){std::ostringstream s;s<<std::hex<<std::setfill('0');for(auto b:u.bytes)s<<std::setw(2)<<static_cast<unsigned>(static_cast<unsigned char>(b));return s.str();}

class GpuWorker {
public:
    CUcontext context=nullptr;cudaDeviceProp prop{};Telemetry* host;Telemetry* device=nullptr;
    cudaStream_t stream=nullptr;cudaEvent_t begin_event=nullptr,end_event=nullptr;
    cudaGraph_t graph=nullptr;cudaGraphExec_t graph_exec=nullptr;
    uint32_t* output=nullptr;unsigned long long* progress=nullptr;unsigned blocks=0;uint64_t iterations=256,heartbeat;
    int active_blocks_per_sm=0;cudaFuncAttributes attributes{};bool diagnostic=false,probe_only=false,progress_correct=false;
    bool use_graph;double solo_us=0,uninstrumented_us=0;std::string cleanup_log;
    std::vector<uint32_t> expected;
    std::vector<uint32_t> short_expected;
    GpuWorker(Telemetry& telemetry,uint64_t target_us,unsigned waves,uint64_t heartbeat_ns,bool graph_mode,uint64_t fixed_iterations=0,bool diagnostic_progress=false,bool minimal_probe=false)
      :host(&telemetry),heartbeat(heartbeat_ns),diagnostic(diagnostic_progress),probe_only(minimal_probe),use_graph(graph_mode){
        CU_OK(cuInit(0));CUdevice dev;CU_OK(cuDeviceGet(&dev,0));
        CU_OK(cuCtxCreate(&context,CU_CTX_MAP_HOST|CU_CTX_SCHED_YIELD,dev));
        CUDA_OK(cudaGetDeviceProperties(&prop,0));
        if(prop.major!=8 || prop.minor!=0)throw std::runtime_error("This prototype targets sm_80/A100; another GPU needs separate validation");
        if(!prop.canMapHostMemory)throw std::runtime_error("Mapped host observation unavailable");
        if(prop.computeMode!=cudaComputeModeDefault)throw std::runtime_error("Two-context experiment requires default compute mode");
        // MPS/MIG/virtualization are assessed by preflight/operator; one env var is not proof.
        blocks=minimal_probe?1:2*prop.multiProcessorCount*waves;
        if(blocks>max_blocks)throw std::runtime_error("CTA telemetry capacity exceeded");
        CUDA_OK(cudaHostRegister(host,sizeof(*host),cudaHostRegisterMapped));
        CUDA_OK(cudaHostGetDevicePointer(&device,host,0));
        CUDA_OK(cudaStreamCreateWithFlags(&stream,cudaStreamNonBlocking));
        CUDA_OK(cudaEventCreate(&begin_event));CUDA_OK(cudaEventCreate(&end_event));
        CUDA_OK(cudaFuncSetAttribute(arithmetic,cudaFuncAttributeMaxDynamicSharedMemorySize,65536));
        CUDA_OK(cudaFuncGetAttributes(&attributes,arithmetic));
        CUDA_OK(cudaOccupancyMaxActiveBlocksPerMultiprocessor(&active_blocks_per_sm,arithmetic,256,65536));
        CUDA_OK(cudaMalloc(&output,output_count()*sizeof(uint32_t)));
        if(diagnostic)CUDA_OK(cudaMalloc(&progress,progress_count()*sizeof(unsigned long long)));
        // No interaction is measured during calibration/reference construction.
        iterations=minimal_probe?256:(fixed_iterations?fixed_iterations:16384);
        for(int pass=0;!minimal_probe&&!fixed_iterations&&pass<4;++pass){
            double elapsed=measure(false);
            if(elapsed<=0)throw std::runtime_error("Invalid CUDA calibration time");
            double scaled=iterations*double(target_us)/elapsed;
            if(scaled>1000000000.0)throw std::runtime_error("Calibration exceeds finite iteration ceiling");
            iterations=std::max<uint64_t>(256,(uint64_t(scaled)+255)/256*256);
        }
        uninstrumented_us=measure(false);
        solo_us=measure(true);
        if(!minimal_probe&&!fixed_iterations&&(solo_us<0.5*target_us || solo_us>1.5*target_us))throw std::runtime_error("Kernel calibration outside target tolerance");
        expected.resize(output_count());CUDA_OK(cudaMemcpy(expected.data(),output,expected.size()*sizeof(uint32_t),cudaMemcpyDeviceToHost));
        reset();
        if(use_graph){
            CUDA_OK(cudaStreamBeginCapture(stream,cudaStreamCaptureModeThreadLocal));enqueue(true);
            CUDA_OK(cudaStreamEndCapture(stream,&graph));CUDA_OK(cudaGraphInstantiate(&graph_exec,graph,0));
            // Materialize any graph-specific CUDA channels before RM discovery.
            reset();launch();
            if(!correct())throw std::runtime_error("Graph warmup differs from ordinary-kernel reference");
            reset();
        }
    }
    size_t output_count()const{return size_t(blocks)*256*(use_graph?3:1);}
    size_t progress_count()const{return size_t(blocks)*2*(use_graph?3:1);}
    void reset(){std::memset(host,0,sizeof(*host));if(progress)CUDA_OK(cudaMemsetAsync(progress,0,progress_count()*sizeof(unsigned long long),stream));}
    void enqueue(bool instrument){
        const unsigned nodes=use_graph?3:1;
        for(unsigned node=0;node<nodes;++node){
            bool main=nodes==1 || node==1;
            uint64_t it=main?iterations:std::max<uint64_t>(256,(iterations/32)/256*256);
            arithmetic<<<blocks,256,65536,stream>>>(output+size_t(node)*blocks*256,it,device,instrument?heartbeat:0,instrument&&main,instrument&&node==0,node,node+1,progress?progress+size_t(node)*blocks*2:nullptr);
        }
        finish_marker<<<1,1,0,stream>>>(device);
    }
    void launch(){if(graph_exec)CUDA_OK(cudaGraphLaunch(graph_exec,stream));else enqueue(true);CUDA_OK(cudaGetLastError());CUDA_OK(cudaEventRecord(end_event,stream));}
    void drain(){
        uint64_t deadline=monotonic_ns()+host_timeout_ns;
        while(true){check_stop();auto e=cudaEventQuery(end_event);if(e==cudaSuccess)break;if(e!=cudaErrorNotReady)cuda_check(e,"cudaEventQuery");if(monotonic_ns()>deadline)throw std::runtime_error("GPU event deadline expired");relax();}
    }
    bool error_drain(const std::string& path)noexcept{
        // Evidence is written before entering CUDA. The runner still bounds
        // the whole owner process if a driver call itself does not return.
        try{
            std::ofstream f(path,std::ios::app);f<<"RECOVERY_UNCONFIRMED: checking finite work event before ordinary cleanup"<<std::endl;
            uint64_t deadline=monotonic_ns()+2000000000ull;
            while(end_event){auto e=cudaEventQuery(end_event);
                if(e==cudaSuccess){f<<"WORK_EVENT_DRAINED; correctness not implied"<<std::endl;return true;}
                if(e!=cudaErrorNotReady){f<<"RECOVERY_UNCONFIRMED: cudaEventQuery code="<<int(e)<<' '<<cudaGetErrorName(e)<<std::endl;return false;}
                if(monotonic_ns()>deadline){f<<"RECOVERY_UNCONFIRMED: bounded drain expired"<<std::endl;return false;}
                relax();
            }
            f<<"WORK_EVENT_UNAVAILABLE"<<std::endl;
        }catch(...){}
        return false;
    }
    bool correct(){
        drain();std::vector<uint32_t> actual(output_count());
        CUDA_OK(cudaMemcpy(actual.data(),output,actual.size()*sizeof(uint32_t),cudaMemcpyDeviceToHost));
        progress_correct=true;
        if(progress){
            std::vector<unsigned long long> counters(progress_count());
            CUDA_OK(cudaMemcpy(counters.data(),progress,counters.size()*sizeof(unsigned long long),cudaMemcpyDeviceToHost));
            for(unsigned node=0;node<(use_graph?3u:1u);++node){
                bool main=!use_graph||node==1;uint64_t chunks=(main?iterations:std::max<uint64_t>(256,(iterations/32)/256*256))/256;
                for(unsigned b=0;b<blocks;++b){auto pos=(size_t(node)*blocks+b)*2;progress_correct&=counters[pos]==chunks&&counters[pos+1]==chunks*(chunks+1)/2;}
            }
        }
        return actual==expected&&progress_correct;
    }
    void prepare_short_reference(){
        if(use_graph||diagnostic)throw std::runtime_error("Short reuse check is plain-kernel only");
        auto saved=iterations;iterations=256;measure(false);short_expected.resize(output_count());
        CUDA_OK(cudaMemcpy(short_expected.data(),output,short_expected.size()*sizeof(uint32_t),cudaMemcpyDeviceToHost));
        iterations=saved;reset();
    }
    bool check_short_reuse(){
        if(short_expected.empty())throw std::runtime_error("Short reference was not prepared before binding");
        auto saved=iterations;iterations=256;reset();launch();drain();
        std::vector<uint32_t> actual(output_count());CUDA_OK(cudaMemcpy(actual.data(),output,actual.size()*sizeof(uint32_t),cudaMemcpyDeviceToHost));
        iterations=saved;return actual==short_expected;
    }
    double measure(bool instrument){
        reset();CUDA_OK(cudaEventRecord(begin_event,stream));enqueue(instrument);CUDA_OK(cudaGetLastError());CUDA_OK(cudaEventRecord(end_event,stream));drain();
        float ms;CUDA_OK(cudaEventElapsedTime(&ms,begin_event,end_event));return ms*1000.0;
    }
    void calibrate_observation(const std::string& path){
        reset();constexpr unsigned count=1000;
        std::ofstream file(path);if(!file)throw std::runtime_error("Cannot write calibration");file<<"sample,cpu_send_ns,gpu_ns,cpu_observed_ns,rtt_ns\n";
        ping_calibration<<<1,1,0,stream>>>(device,count);CUDA_OK(cudaGetLastError());
        CUDA_OK(cudaEventRecord(end_event,stream));
        // No syscalls/file writes inside the handshake loop.
        struct Row{uint64_t a,g,b;};std::vector<Row> rows(count);
        for(unsigned i=1;i<=count;++i){rows[i-1].a=monotonic_ns();release(&host->ping_request,i);wait_value(&host->ping_ack,i,"mapped ping response");rows[i-1].b=monotonic_ns();rows[i-1].g=host->ping_gpu_ns;}
        drain();for(unsigned i=0;i<count;++i)file<<i<<','<<rows[i].a<<','<<rows[i].g<<','<<rows[i].b<<','<<rows[i].b-rows[i].a<<'\n';reset();
    }
    bool shutdown()noexcept{
        bool ok=true;
        try{
            std::ofstream file;if(!cleanup_log.empty())file.open(cleanup_log,std::ios::app);
            auto result=[&](cudaError_t e,const char* name){
                if(e!=cudaSuccess)ok=false;
                if(file)file<<name<<" code="<<int(e)<<" "<<cudaGetErrorName(e)<<" "<<cudaGetErrorString(e)<<std::endl;
            };
            if(graph_exec){result(cudaGraphExecDestroy(graph_exec),"cudaGraphExecDestroy");graph_exec=nullptr;}
            if(graph){result(cudaGraphDestroy(graph),"cudaGraphDestroy");graph=nullptr;}
            if(progress){result(cudaFree(progress),"cudaFree(progress)");progress=nullptr;}
            if(output){result(cudaFree(output),"cudaFree(output)");output=nullptr;}
            if(end_event){result(cudaEventDestroy(end_event),"cudaEventDestroy(end)");end_event=nullptr;}
            if(begin_event){result(cudaEventDestroy(begin_event),"cudaEventDestroy(begin)");begin_event=nullptr;}
            if(stream){result(cudaStreamDestroy(stream),"cudaStreamDestroy");stream=nullptr;}
            if(device){result(cudaHostUnregister(host),"cudaHostUnregister");device=nullptr;}
            if(context){auto e=cuCtxDestroy(context);if(e!=CUDA_SUCCESS)ok=false;if(file)file<<"cuCtxDestroy code="<<int(e)<<std::endl;context=nullptr;}
        }catch(...){ok=false;}
        return ok;
    }
    ~GpuWorker(){shutdown();} // ordinary cleanup only; never a preemption trigger

};
inline void write_identity(std::ostream& out,const GpuWorker& w,const RmControl* rm=nullptr){
    out<<"pid="<<getpid()<<" context="<<reinterpret_cast<uintptr_t>(w.context)<<" gpu_uuid="<<uuid_string(w.prop.uuid)<<" device="<<w.prop.name<<"\n";
    if(rm){auto& i=rm->identity();out<<"client_generation="<<i.binding.client_generation<<" group_generation="<<i.binding.group.generation<<" hDevice="<<i.device<<"\n";
    out<<"hClient="<<i.client<<" hTSG="<<i.group<<" tsgID="<<i.tsg_id<<" engine="<<i.engine<<" subdevice="<<i.subdevice<<" compute_channel="<<i.compute_channel<<" channels=";
    for(auto ch:i.channels)out<<ch<<',';out<<"\n";}else out<<"strict_channel_identity=not_bound; group identity, if present, is recorded separately\n";
    out<<"hardware_channel_id=unknown runlist=unknown scheduling_policy=unknown MPS_MIG_virtualization=see_preflight\n";
    out<<"block=256 shared_dynamic=65536 registers="<<w.attributes.numRegs<<" shared_static="<<w.attributes.sharedSizeBytes<<" active_blocks_per_sm_estimate="<<w.active_blocks_per_sm<<" occupancy_is_estimate=1 diagnostic_progress="<<w.diagnostic<<"\n";
    out<<"iterations="<<w.iterations<<" blocks="<<w.blocks<<" solo_us="<<w.solo_us<<" uninstrumented_us="<<w.uninstrumented_us<<"\n";
}
struct OwnedGroup {GroupIdentity identity;GroupInfoResult info;};
inline OwnedGroup bind_worker_group(const GpuWorker& gpu,const std::string& directory,const std::string& owner){
    int count=0;CUDA_OK(cudaGetDeviceCount(&count));note_group_cuda_scope(uuid_string(gpu.prop.uuid),count);
    save_rm_observation(directory,owner+"_before_binding_");
    auto id=inspect_owned_tsg();
    std::ofstream identity(directory+"/"+owner+"_group_identity.json");identity<<id.json()<<'\n';identity.close();
    if(!identity)throw std::runtime_error("Cannot save current group identity");
    auto info=get_group_info(id);save_control_journal();
    const auto& r=info.control;
    std::ofstream result(directory+"/"+owner+"_group_get_info.json");
    result<<std::boolalpha<<"{\"pid\":"<<getpid()<<",\"hClient\":"<<id.binding().client<<",\"hObject\":"<<id.binding().group.token.handle
          <<",\"attempted\":"<<r.attempted<<",\"syscall_return\":"<<(r.attempted?std::to_string(r.syscall_result):"null")
          <<",\"errno\":"<<(r.attempted?std::to_string(r.syscall_errno):"null")<<",\"NV_STATUS_raw\":"<<r.rm_status<<",\"operation_seq\":"<<r.operation_seq<<",\"verified\":"<<r.ok()
          <<",\"call_begin_ns\":"<<(r.begin_ns?std::to_string(r.begin_ns):"null")<<",\"call_end_ns\":"<<(r.end_ns?std::to_string(r.end_ns):"null")
          <<",\"hardware_tsg_id\":"<<(info.hardware_tsg_id?std::to_string(*info.hardware_tsg_id):"null")<<",\"result\":"<<json_string(r.describe())<<"}\n";
    result.close();save_rm_observation(directory,owner+"_after_binding_");
    require_ok(r,"group GET_INFO");if(!result)throw std::runtime_error("Cannot save group GET_INFO result");
    if(!verified_group_current(id))throw std::runtime_error("OBJECT_BINDING_UNAVAILABLE: group changed after GET_INFO");
    return {std::move(id),info};
}
inline void write_group_identity(std::ostream& out,const OwnedGroup& group){
    const auto& b=group.identity.binding();
    out<<"group_identity=verified hClient="<<b.client<<" hTSG="<<b.group.token.handle<<" group_generation="<<b.group.token.generation
       <<" hardware_tsg_id="<<*group.info.hardware_tsg_id<<" engine="<<b.group.engine<<" captured_channels="<<b.members.size()<<'\n'
       <<"channel_preemption_mode=NOT_MEASURED:CHANNEL_BINDING_UNAVAILABLE runlist_id=unknown\n";
}
}
