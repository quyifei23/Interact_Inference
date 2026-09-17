#pragma once
#include "protocol.h"
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
inline void cuda_check(cudaError_t e,const char* op){if(e!=cudaSuccess)throw std::runtime_error(std::string(op)+": "+cudaGetErrorString(e));}
inline void cu_check(CUresult e,const char* op){if(e!=CUDA_SUCCESS){const char* msg=nullptr;cuGetErrorString(e,&msg);throw std::runtime_error(std::string(op)+": "+(msg?msg:"CUDA driver error"));}}
#define CUDA_OK(x) ::ap::cuda_check((x),#x)
#define CU_OK(x) ::ap::cu_check((x),#x)
__device__ inline uint64_t global_ns(){uint64_t t;asm volatile("mov.u64 %0, %%globaltimer;":"=l"(t));return t;}
__device__ inline void publish(uint32_t* p,uint32_t v){__threadfence_system();asm volatile("st.release.sys.global.u32 [%0], %1;"::"l"(p),"r"(v):"memory");}
__device__ inline uint32_t device_acquire(uint32_t* p){uint32_t v;asm volatile("ld.acquire.sys.global.u32 %0, [%1];":"=r"(v):"l"(p):"memory");return v;}

// Finite integer arithmetic with live registers and shared-memory dependencies.
// No sleep, spin-wait, input-dependent unbounded loop, or cancellation polling.
// The 2-second wall-time guard invalidates the sample if it ever fires.
static __global__ void arithmetic(uint32_t* output,uint64_t iterations,Telemetry* t,
                                  uint64_t heartbeat_ns,bool measured,uint32_t seed) {
    extern __shared__ uint32_t memory[];
    uint32_t lane=threadIdx.x,idx=blockIdx.x*blockDim.x+lane;
    uint32_t x=idx*747796405u+seed,y=idx^0x9e3779b9u;
    __shared__ uint32_t expired;
    uint64_t begin=global_ns(),last=begin;
    uint32_t samples=0;
    if(lane==0){expired=0;if(measured)t->cta_start_gpu_ns[blockIdx.x]=begin;}
    if(idx==0 && measured){t->start_gpu_ns=begin;publish(&t->started,1);}
    for(uint64_t round=0;round<iterations;round+=256) {
        #pragma unroll 1
        for(unsigned k=0;k<256;++k){x=(x*1664525u+1013904223u)^y;y=(y<<7)|(y>>25);y+=x^0xa511e9b3u;}
        memory[lane]=x;
        __syncthreads();
        x^=memory[(lane+1)%blockDim.x];
        __syncthreads();
        if(lane==0){
            uint64_t now=global_ns();
            if(now-begin>gpu_watchdog_ns){expired=1;t->watchdog[blockIdx.x]=1;}
            if(idx==0 && measured && heartbeat_ns && now-last>=heartbeat_ns){
                if(samples<max_samples){t->heartbeat_gpu_ns[samples++]=now;publish(&t->heartbeat_count,samples);}
                else t->overflow=1;
                last=now;
            }
        }
        __syncthreads();
        if(expired)break;
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

struct Options {
    std::string mode="none",run_dir="",shared_path="";
    unsigned trials=1000,waves=1;uint64_t bg_us=80000,int_us=300,heartbeat_ns=2000,timeslice_us=1;
    bool force=false,bypass=false,graph=false,probe=false;
};
inline Options parse(int argc,char** argv){
    Options o;
    for(int i=1;i<argc;++i){std::string key=argv[i];
        if(key=="--graph")o.graph=true;
        else if(key=="--probe")o.probe=true;
        else if(key=="--help"){
            std::cout<<"int_worker --run-dir DIR [--mode none|timeslice|preempt-wait|preempt-async|realtime|disable|disable-split] [--trials 1000] [--bg-us 80000] [--int-us 300] [--cta-waves 1] [--force 0|1] [--bypass 0|1] [--heartbeat-ns 2000] [--timeslice-us 1] [--graph]\n";
            std::exit(0);
        } else {
            if(++i>=argc)throw std::invalid_argument("Missing value for "+key);
            std::string value=argv[i];
            if(key=="--run-dir")o.run_dir=value;
            else if(key=="--shared")o.shared_path=value;
            else if(key=="--mode")o.mode=value;
            else {size_t used=0;uint64_t v=std::stoull(value,&used);if(used!=value.size() || value[0]=='-')throw std::invalid_argument("Invalid numeric option");
                if(key=="--trials"){if(v>100000)throw std::invalid_argument("Too many trials");o.trials=v;}
                else if(key=="--cta-waves"){if(v>16)throw std::invalid_argument("Too many CTA waves");o.waves=v;}
                else if(key=="--bg-us")o.bg_us=v;
                else if(key=="--int-us")o.int_us=v;
                else if(key=="--heartbeat-ns")o.heartbeat_ns=v;
                else if(key=="--timeslice-us")o.timeslice_us=v;
                else if(key=="--force" || key=="--bypass"){if(v>1)throw std::invalid_argument("Boolean option requires 0 or 1");if(key=="--force")o.force=v;else o.bypass=v;}
                else throw std::invalid_argument("Unknown option "+key);
            }
        }
    }
    if(!o.trials || !o.waves || o.bg_us<50000 || o.bg_us>100000 || o.int_us<100 || o.int_us>500 || o.heartbeat_ns>1000000 || o.timeslice_us==0 || o.timeslice_us>1000000)
        throw std::invalid_argument("Bounds: BG 50-100ms, INT 100-500us, waves 1-16, nonzero trials/timeslice");
    const std::vector<std::string> modes={"none","timeslice","preempt-wait","preempt-async","realtime","disable","disable-split"};
    if(std::find(modes.begin(),modes.end(),o.mode)==modes.end())throw std::invalid_argument("Unknown mode");
    return o;
}
inline std::string uuid_string(const cudaUUID_t& u){std::ostringstream s;s<<std::hex<<std::setfill('0');for(auto b:u.bytes)s<<std::setw(2)<<static_cast<unsigned>(static_cast<unsigned char>(b));return s.str();}

class GpuWorker {
public:
    CUcontext context=nullptr;cudaDeviceProp prop{};Telemetry* host;Telemetry* device=nullptr;
    cudaStream_t stream=nullptr;cudaEvent_t begin_event=nullptr,end_event=nullptr;
    cudaGraph_t graph=nullptr;cudaGraphExec_t graph_exec=nullptr;
    uint32_t* output=nullptr;unsigned blocks=0;uint64_t iterations=256,heartbeat;
    bool use_graph;double solo_us=0,uninstrumented_us=0;
    std::vector<uint32_t> expected;
    GpuWorker(Telemetry& telemetry,uint64_t target_us,unsigned waves,uint64_t heartbeat_ns,bool graph_mode)
      :host(&telemetry),heartbeat(heartbeat_ns),use_graph(graph_mode){
        CU_OK(cuInit(0));CUdevice dev;CU_OK(cuDeviceGet(&dev,0));
        CU_OK(cuCtxCreate(&context,CU_CTX_MAP_HOST|CU_CTX_SCHED_YIELD,dev));
        CUDA_OK(cudaGetDeviceProperties(&prop,0));
        if(prop.major!=8 || prop.minor!=0)throw std::runtime_error("This prototype targets sm_80/A100; another GPU needs separate validation");
        if(!prop.canMapHostMemory)throw std::runtime_error("Mapped host observation unavailable");
        if(prop.computeMode!=cudaComputeModeDefault)throw std::runtime_error("Two-context experiment requires default compute mode");
        if(std::getenv("CUDA_MPS_PIPE_DIRECTORY"))throw std::runtime_error("MPS experiment is out of scope");
        blocks=2*prop.multiProcessorCount*waves;
        if(blocks>max_blocks)throw std::runtime_error("CTA telemetry capacity exceeded");
        CUDA_OK(cudaHostRegister(host,sizeof(*host),cudaHostRegisterMapped));
        CUDA_OK(cudaHostGetDevicePointer(&device,host,0));
        CUDA_OK(cudaStreamCreateWithFlags(&stream,cudaStreamNonBlocking));
        CUDA_OK(cudaEventCreate(&begin_event));CUDA_OK(cudaEventCreate(&end_event));
        CUDA_OK(cudaFuncSetAttribute(arithmetic,cudaFuncAttributeMaxDynamicSharedMemorySize,65536));
        CUDA_OK(cudaMalloc(&output,output_count()*sizeof(uint32_t)));
        // No interaction is measured during calibration/reference construction.
        iterations=16384;
        for(int pass=0;pass<4;++pass){
            double elapsed=measure(false);
            if(elapsed<=0)throw std::runtime_error("Invalid CUDA calibration time");
            double scaled=iterations*double(target_us)/elapsed;
            if(scaled>1000000000.0)throw std::runtime_error("Calibration exceeds finite iteration ceiling");
            iterations=std::max<uint64_t>(256,(uint64_t(scaled)+255)/256*256);
        }
        uninstrumented_us=measure(false);
        solo_us=measure(true);
        if(solo_us<0.5*target_us || solo_us>1.5*target_us)throw std::runtime_error("Kernel calibration outside target tolerance");
        expected.resize(output_count());CUDA_OK(cudaMemcpy(expected.data(),output,expected.size()*sizeof(uint32_t),cudaMemcpyDeviceToHost));
        validate_watchdog();
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
    void reset(){std::memset(host,0,sizeof(*host));}
    void enqueue(bool instrument){
        const unsigned nodes=use_graph?3:1;
        for(unsigned node=0;node<nodes;++node){
            bool main=nodes==1 || node==1;
            uint64_t it=main?iterations:std::max<uint64_t>(256,(iterations/32)/256*256);
            arithmetic<<<blocks,256,65536,stream>>>(output+size_t(node)*blocks*256,it,device,instrument?heartbeat:0,instrument&&main,node+1);
        }
        finish_marker<<<1,1,0,stream>>>(device);
    }
    void launch(){if(graph_exec)CUDA_OK(cudaGraphLaunch(graph_exec,stream));else enqueue(true);CUDA_OK(cudaGetLastError());CUDA_OK(cudaEventRecord(end_event,stream));}
    void drain(){
        uint64_t deadline=monotonic_ns()+host_timeout_ns;
        while(true){auto e=cudaEventQuery(end_event);if(e==cudaSuccess)break;if(e!=cudaErrorNotReady)cuda_check(e,"cudaEventQuery");if(monotonic_ns()>deadline)throw std::runtime_error("GPU event deadline expired");relax();}
        validate_watchdog();
    }
    void validate_watchdog(){for(unsigned i=0;i<blocks;++i)if(host->watchdog[i])throw std::runtime_error("Finite kernel watchdog fired; result invalid");}
    bool correct(){
        drain();std::vector<uint32_t> actual(output_count());
        CUDA_OK(cudaMemcpy(actual.data(),output,actual.size()*sizeof(uint32_t),cudaMemcpyDeviceToHost));
        return actual==expected;
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
    ~GpuWorker(){
        // Normal process cleanup only. Never used as the preemption primitive.
        if(graph_exec)cudaGraphExecDestroy(graph_exec);if(graph)cudaGraphDestroy(graph);
        if(output)cudaFree(output);if(end_event)cudaEventDestroy(end_event);if(begin_event)cudaEventDestroy(begin_event);
        if(stream)cudaStreamDestroy(stream);if(device)cudaHostUnregister(host);if(context)cuCtxDestroy(context);
    }
};
inline void write_identity(std::ostream& out,const GpuWorker& w,const RmControl& rm){
    auto& i=rm.identity();out<<"pid="<<getpid()<<" context="<<reinterpret_cast<uintptr_t>(w.context)<<" gpu_uuid="<<uuid_string(w.prop.uuid)<<" device="<<w.prop.name<<"\n";
    out<<"hClient="<<i.client<<" hTSG="<<i.group<<" tsgID="<<i.tsg_id<<" engine="<<i.engine<<" subdevice="<<i.subdevice<<" compute_channel="<<i.compute_channel<<" channels=";
    for(auto ch:i.channels)out<<ch<<',';out<<"\niterations="<<w.iterations<<" blocks="<<w.blocks<<" solo_us="<<w.solo_us<<" uninstrumented_us="<<w.uninstrumented_us<<"\n";
}
}
