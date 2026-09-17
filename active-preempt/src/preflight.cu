// Read-only with respect to driver/system configuration. The sole GPU action is
// a bounded, ordinary arithmetic kernel; no scheduling RM controls are issued.
#include "json_log.h"
#include <cuda.h>
#include <cuda_runtime.h>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <chrono>
#include <thread>
#include <iomanip>
#include <unistd.h>
namespace {
void record(const char* call,int code,const std::string& detail) {
    std::cout << "{\"call\":" << ap::json_string(call) << ",\"returncode\":" << code
              << ",\"detail\":" << ap::json_string(detail) << "}" << std::endl;
}
bool driver(CUresult r,const char* call) {
    const char *name=nullptr,*message=nullptr;cuGetErrorName(r,&name);cuGetErrorString(r,&message);
    record(call,int(r),std::string(name?name:"unknown")+": "+(message?message:"unknown"));return r==CUDA_SUCCESS;
}
bool runtime(cudaError_t r,const char* call) {
    record(call,int(r),std::string(cudaGetErrorName(r))+": "+cudaGetErrorString(r));return r==cudaSuccess;
}
__global__ void minimal(unsigned* out) {
    unsigned x=threadIdx.x+1;
    for(unsigned i=0;i<256;++i)x=x*1664525u+1013904223u;
    out[threadIdx.x]=x;
}
}
int main() {
    Dl_info info{};
    if(dladdr(reinterpret_cast<void*>(cuInit),&info))record("dladdr(cuInit)",0,info.dli_fname?info.dli_fname:"unknown");
    record("compile_versions",0,"CUDA_VERSION="+std::to_string(CUDA_VERSION)+" CUDART_VERSION="+std::to_string(CUDART_VERSION));
    int v=0;if(driver(cuDriverGetVersion(&v),"cuDriverGetVersion"))record("driver_api_version",0,std::to_string(v));
    if(runtime(cudaRuntimeGetVersion(&v),"cudaRuntimeGetVersion"))record("runtime_version",0,std::to_string(v));
    bool initialized=driver(cuInit(0),"cuInit(0)");
    int count=0;bool enumerated=driver(cuDeviceGetCount(&count),"cuDeviceGetCount");
    record("device_count",enumerated?0:77,enumerated?std::to_string(count):"unknown (enumeration failed)");
    std::ifstream maps("/proc/self/maps");std::string line;
    while(std::getline(maps,line))if(line.find("libcuda")!=std::string::npos)record("loaded_library_mapping",0,line);
    if(!initialized||!enumerated||count<1){record("minimal_kernel",77,"NOT_RUN: CUDA initialization/enumeration unavailable");return 77;}
    for(int i=0;i<count;++i) {
        CUdevice dev; if(!driver(cuDeviceGet(&dev,i),"cuDeviceGet"))continue;
        char name[256]{};if(driver(cuDeviceGetName(name,sizeof(name),dev),"cuDeviceGetName"))record("device_name",0,name);
        CUuuid uuid{};if(driver(cuDeviceGetUuid(&uuid,dev),"cuDeviceGetUuid")){
            std::ostringstream s;for(unsigned char c:uuid.bytes)s<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(c);record("gpu_uuid",0,s.str());
        }
        struct Attr {CUdevice_attribute id;const char* name;};
        for(auto a:{Attr{CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR,"compute_capability_major"},
                    Attr{CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR,"compute_capability_minor"},
                    Attr{CU_DEVICE_ATTRIBUTE_CAN_MAP_HOST_MEMORY,"can_map_host_memory"},
                    Attr{CU_DEVICE_ATTRIBUTE_HOST_NATIVE_ATOMIC_SUPPORTED,"host_native_atomic_supported"},
                    Attr{CU_DEVICE_ATTRIBUTE_COMPUTE_MODE,"compute_mode"},
                    Attr{CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT,"multiprocessor_count"}}){
            int value=0;if(driver(cuDeviceGetAttribute(&value,a.id,dev),a.name))record(a.name,0,std::to_string(value));
        }
    }
    if(!runtime(cudaSetDevice(0),"cudaSetDevice(0)"))return 77;
    unsigned* output=nullptr;cudaStream_t stream=nullptr;cudaEvent_t event=nullptr;
    if(!runtime(cudaMalloc(&output,32*sizeof(unsigned)),"cudaMalloc"))return 77;
    if(!runtime(cudaStreamCreateWithFlags(&stream,cudaStreamNonBlocking),"cudaStreamCreateWithFlags"))return 77;
    if(!runtime(cudaEventCreateWithFlags(&event,cudaEventDisableTiming),"cudaEventCreateWithFlags"))return 77;
    minimal<<<1,32,0,stream>>>(output);
    if(!runtime(cudaGetLastError(),"minimal<<<1,32>>> launch"))return 77;
    if(!runtime(cudaEventRecord(event,stream),"cudaEventRecord"))return 77;
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);size_t not_ready=0;cudaError_t result;
    while((result=cudaEventQuery(event))==cudaErrorNotReady){
        ++not_ready;if(std::chrono::steady_clock::now()>deadline){record("cudaEventQuery",int(result),"TIMEOUT after 5s; polls="+std::to_string(not_ready));std::cout.flush();_exit(77);}
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    record("cudaEventQuery_not_ready_count",0,std::to_string(not_ready));if(!runtime(result,"cudaEventQuery final"))return 77;
    unsigned actual[32]{};if(!runtime(cudaMemcpy(actual,output,sizeof(actual),cudaMemcpyDeviceToHost),"cudaMemcpy"))return 77;
    bool correct=true;for(unsigned i=0;i<32;++i){unsigned x=i+1;for(unsigned k=0;k<256;++k)x=x*1664525u+1013904223u;correct&=actual[i]==x;}
    record("minimal_kernel_correctness",correct?0:1,correct?"PASS: fixed integer reference":"CORRECTNESS_FAILURE");
    bool cleaned=runtime(cudaEventDestroy(event),"cudaEventDestroy");
    cleaned&=runtime(cudaStreamDestroy(stream),"cudaStreamDestroy");cleaned&=runtime(cudaFree(output),"cudaFree");
    return correct&&cleaned?0:1;
}
