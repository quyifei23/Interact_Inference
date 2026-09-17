#!/usr/bin/env python3
"""Read-only Phase 7 tool inventory. Tool presence is not event availability."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
from preflight import command

def inventory(output):
    output.mkdir(parents=True,exist_ok=False)
    nsys=shutil.which('nsys')
    calls={}
    if nsys:
        for name,argv in [('version',['--version']),('profile_help',['profile','--help']),
                          ('export_help',['export','--help']),('status_help',['status','--help']),
                          ('environment',['status','--environment'])]:
            calls[name]=command([nsys]+argv)
            (output/(name+'.json')).write_text(json.dumps(calls[name],indent=2)+'\n')
    cupti=[]
    roots=[Path('/usr/local/cuda/extras/CUPTI'),Path('/usr/local/lib/python3.10/dist-packages/nvidia/cu13')]
    for root in roots:
        header=root/'include/cupti_activity.h';version=root/'include/cupti_version.h'
        if not header.exists():continue
        item={'header':str(header.resolve()),'header_sha256':hashlib.sha256(header.read_bytes()).hexdigest(),
              'has_compute_engine_ctx_switch':'CUPTI_ACTIVITY_KIND_COMPUTE_ENGINE_CTX_SWITCH' in header.read_text(),
              'api_version_header':re.findall(r'#define CUPTI_API_VERSION\s+(\d+)',version.read_text()),
              'enable_attempted':False,'records_received':None}
        library=next(iter(sorted(root.glob('lib*/libcupti.so*'))),None)
        if library:
            item['library']=str(library.resolve())
            # GetVersion only. This probe is a separate process; never co-load
            # a custom subscriber into a Nsight-injected worker.
            try:
                lib=ctypes.CDLL(str(library));v=ctypes.c_uint32()
                lib.cuptiGetVersion.argtypes=[ctypes.POINTER(ctypes.c_uint32)];lib.cuptiGetVersion.restype=ctypes.c_int
                item['get_version_returncode']=lib.cuptiGetVersion(ctypes.byref(v));item['api_version_loaded']=v.value
            except OSError as e:item['error']=str(e)
        cupti.append(item)
    help_text=calls.get('profile_help',{}).get('stdout','')
    result={'run_kind':'diagnostic','nsys':nsys,'version':calls.get('version'),
            'gpuctxsw_option_present':'--gpuctxsw' in help_text,
            'event_collection_verified':False,'bg_int_association_verified':False,
            'scope':'gpuctxsw is system scope in this installed CLI; CUDA/NVTX follow the application process tree',
            'raw_export_options':'sqlite/text/json reported by installed export help; verify actual report schema',
            'clock':'exported trace clock/origin must be read from report; not CPU RAW or GPU globaltimer by unit alone',
            'permissions':'No changes. CPU perf probe failure is not proof of GPU context-switch denial. Test actual collection.',
            'cupti':cupti,'uid':os.getuid(),
            'process_security':[x for x in Path('/proc/self/status').read_text().splitlines() if x.startswith(('Cap','NoNewPrivs','Seccomp'))],
            'driver':command(['cat','/proc/driver/nvidia/version']),
            'profiling_parameter':[x for x in Path('/proc/driver/nvidia/params').read_text().splitlines() if 'Profiling' in x] if Path('/proc/driver/nvidia/params').exists() else 'unknown'}
    (output/'probe.json').write_text(json.dumps(result,indent=2)+'\n')
    return result

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    r=inventory(a.output);print(json.dumps({k:r[k] for k in ('nsys','gpuctxsw_option_present','event_collection_verified','cupti')},indent=2))
