#!/usr/bin/env python3
"""Record read-only readiness probes; never change GPU configuration or privileges."""
import argparse
import ctypes
import datetime
import glob
import json
import os
from pathlib import Path
import subprocess

def command(argv, timeout=20):
    try:
        p=subprocess.run(argv,capture_output=True,text=True,timeout=timeout)
        return dict(call=argv,returncode=p.returncode,stdout=p.stdout,stderr=p.stderr)
    except (OSError,subprocess.TimeoutExpired) as e:
        return dict(call=argv,returncode=None,errno=getattr(e,'errno',None),error=str(e))

def collect(binary):
    result={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'uid':os.getuid(),'euid':os.geteuid(),
            'gid':os.getgid(),'groups':os.getgroups(),'scheduling_controls_issued':0,'performance_samples':0}
    result['commands']=[command(c) for c in (
        ['git','rev-parse','HEAD'],['git','status','--porcelain'],['uname','-a'],
        ['cat','/proc/driver/nvidia/version'],['nvidia-smi','-q'],['nvidia-smi','-L'],
        ['nvidia-smi','--query-compute-apps=pid,process_name,gpu_uuid','--format=csv,noheader'],
        ['nvcc','--version'],['capsh','--print'],['systemd-detect-virt'],
        ['ps','-eo','pid,comm'],['cat','/proc/driver/nvidia/params'])]
    result['process_security']=[x for x in Path('/proc/self/status').read_text().splitlines()
        if x.split(':')[0] in {'Uid','Gid','CapInh','CapPrm','CapEff','CapBnd','CapAmb','NoNewPrivs','Seccomp'}]
    result['cuda_environment']={k:os.environ[k] for k in ('CUDA_VISIBLE_DEVICES','CUDA_MPS_PIPE_DIRECTORY','CUDA_MPS_ACTIVE_THREAD_PERCENTAGE','NVIDIA_VISIBLE_DEVICES') if k in os.environ}
    result['nodes']=[]
    for name in sorted(set(['/dev/nvidiactl','/dev/nvidia0','/dev/nvidia-uvm']+glob.glob('/dev/nvidia*'))):
        item={'path':name}
        try:
            st=os.stat(name);item.update(mode=oct(st.st_mode),uid=st.st_uid,gid=st.st_gid)
            if not os.path.isdir(name):
                fd=os.open(name,os.O_RDWR|os.O_CLOEXEC);os.close(fd);item['open_returncode']=0
        except OSError as e:item.update(open_returncode=-1,errno=e.errno,error=str(e))
        result['nodes'].append(item)
    # Independent of nvidia-smi and device-node checks. Dynamic loader errors,
    # cuInit, enumeration and skipped kernel reasons remain separate evidence.
    result['cuda_probe']=command([str(binary.resolve())],30)
    result['cupti']={'status':'OBSERVABILITY_UNAVAILABLE'}
    for candidate in ['libcupti.so','/usr/local/cuda/extras/CUPTI/lib64/libcupti.so']:
        try:
            lib=ctypes.CDLL(candidate);fn=lib.cuptiGetVersion;fn.argtypes=[ctypes.POINTER(ctypes.c_uint32)];fn.restype=ctypes.c_int
            v=ctypes.c_uint32();code=fn(ctypes.byref(v))
            result['cupti']=dict(call='cuptiGetVersion',library=candidate,returncode=code,api_version=v.value)
            result['cupti']['loaded_paths']=sorted(set(x.split()[-1] for x in Path('/proc/self/maps').read_text().splitlines() if 'libcupti' in x))
            break
        except (OSError,AttributeError) as e:result['cupti'].setdefault('attempts',[]).append({'library':candidate,'error':str(e)})
    header=Path('/usr/local/cuda/extras/CUPTI/include/cupti_activity.h')
    result['cupti']['header']=str(header)
    result['cupti']['header_has_compute_engine_ctx_switch']=('CUPTI_ACTIVITY_KIND_COMPUTE_ENGINE_CTX_SWITCH' in header.read_text()) if header.is_file() else None
    result['cupti']['trace_backend']='not_enabled_in_phase2'
    result['gsp_mig_virtualization']='See nvidia-smi -q fields; unknown if query failed'
    result['mps']='process/environment hints only; absence is not proof MPS is off'
    result['cuda_usable']=result['cuda_probe'].get('returncode')==0
    ctl=next(x for x in result['nodes'] if x['path']=='/dev/nvidiactl')
    result['rm_device_accessible']=ctl.get('open_returncode')==0
    result['readiness']='CUDA_PROBE_PASSED_BINDING_STILL_REQUIRED' if result['cuda_usable'] else 'DEVICE_NOT_ACCESSIBLE'
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--binary',type=Path,default=Path(__file__).resolve().parents[1]/'build/preflight_cuda');p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    a.output.mkdir(parents=True,exist_ok=False)
    r=collect(a.binary);(a.output/'preflight.json').write_text(json.dumps(r,indent=2)+'\n')
    (a.output/'cuda_probe.jsonl').write_text(r['cuda_probe'].get('stdout',''))
    (a.output/'summary.md').write_text(f"Readiness: {r['readiness']}\n\nCUDA probe exit: {r['cuda_probe'].get('returncode')}; RM scheduling controls: 0; performance samples: 0.\n\nSee preflight.json for calls, return values and errors. This is a readiness probe, not a scheduling experiment.\n")
    print(r['readiness']);return 0 if r['cuda_usable'] else 77

if __name__=='__main__':raise SystemExit(main())
