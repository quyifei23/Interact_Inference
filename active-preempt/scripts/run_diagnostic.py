#!/usr/bin/env python3
"""One Nsight D0 or D1. No retries, kernel replay, permission changes or automatic D1."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import sys
import time
from preflight import collect,command
from run_matrix import binary_fingerprints,check_compute_processes,run_process_group
from run_paired import control_facts,validate_run,recover_journals

FROZEN={'bg_iterations':1630976,'int_iterations':5888,'cta_waves':1,'heartbeat_ns':2000,'trigger_delay_us':3000}
ENV_KEYS=('PATH','HOME','USER','LOGNAME','LANG','LC_ALL','LD_LIBRARY_PATH','TMPDIR','XDG_RUNTIME_DIR')

def profiler_environment(source,gpu):
    # Nsight embeds the launched environment in opaque reports. Do not copy
    # tokens/credentials or unrelated agent/SSH configuration into evidence.
    # Reject rather than silently changing a CUDA/MPS tuning configuration.
    unexpected=[k for k in source if k.startswith(('CUDA_','NVIDIA_')) and k!='CUDA_VISIBLE_DEVICES']
    if unexpected:raise ValueError('Review existing CUDA environment first: '+','.join(unexpected))
    env={k:source[k] for k in ENV_KEYS if k in source};env['CUDA_VISIBLE_DEVICES']=gpu
    return env

def belongs_to_run(argv,run_dir):
    if not argv or Path(argv[0]).name not in ('int_worker','bg_worker'):return False
    return any(argv[i]=='--run-dir' and argv[i+1]==str(run_dir) for i in range(len(argv)-1))

def cleanup_escaped_owners(run_dir):
    """Nsight may create a new process session. Bound cleanup to this run's owners.

    A survivor is always RECOVERY_UNCONFIRMED even if termination succeeds.
    pidfds prevent signals being delivered to a recycled PID; no other task's
    processes, GPU state or privileges are touched.
    """
    handles=[];result=[]
    for path in Path('/proc').iterdir():
        if not path.name.isdecimal():continue
        try:
            if path.stat().st_uid!=os.getuid():continue
            fd=os.pidfd_open(int(path.name))
            try:
                argv=[x.decode(errors='replace') for x in (path/'cmdline').read_bytes().split(b'\0') if x]
                if belongs_to_run(argv,run_dir):handles.append(fd);result.append({'pid':int(path.name),'state':'RECOVERY_UNCONFIRMED','actions':[]});fd=None
            finally:
                if fd is not None:os.close(fd)
        except (FileNotFoundError,ProcessLookupError,PermissionError):continue
    if handles:
        for fd,r in zip(handles,result):
            try:signal.pidfd_send_signal(fd,signal.SIGTERM);r['actions'].append('SIGTERM_OWNER_CLEANUP')
            except ProcessLookupError:pass
        time.sleep(3) # bounded opportunity for existing owner-side drain/cleanup
        for fd,r in zip(handles,result):
            try:signal.pidfd_send_signal(fd,signal.SIGKILL);r['actions'].append('SIGKILL_RECOVERY_UNCONFIRMED')
            except ProcessLookupError:pass
            finally:os.close(fd)
    return result

def write(path,data):path.write_text(json.dumps(data,indent=2)+'\n')

def profile_command(nsys,build,output,gpu,condition,confirmed=False):
    if condition not in ('D0','D1'):raise ValueError('Only D0/D1 supported')
    if condition=='D1' and not confirmed:raise ValueError('D1 requires current explicit host authorization')
    if condition=='D0' and confirmed:raise ValueError('D0 must remain unprivileged/non-active')
    mode='group-bound-none' if condition=='D0' else 'group-preempt-wait'
    cmd=[nsys,'profile','--trace=cuda,nvtx','--gpuctxsw=true','--sample=none','--cpuctxsw=none',
         '--trace-fork-before-exec=false','--wait=all','--kill=none','--stop-on-exit=true','--duration=0','--show-output=true',
         '--export=none','--force-overwrite=false','--output='+str(output/'trace'),
         '--env-var=LD_PRELOAD='+str(build/'librm_control.so'),str(build/'int_worker'),
         '--run-dir',str(output/'worker'),'--mode',mode,'--trials','1','--diagnostic-trace','--run-id',output.name]
    for key,value in FROZEN.items():cmd+=['--'+key.replace('_','-'),str(value)]
    if condition=='D1':cmd+=['--test-host-confirmed']
    return cmd

def validate_d0(path,build,gpu):
    analysis=json.loads((path/'analysis/assessment.json').read_text())
    if analysis.get('backend_ready_for_d1') is not True:raise ValueError('D0 lacks usable associated context-switch evidence; no active request')
    old=json.loads((path/'invocation.json').read_text())
    if old['condition']!='D0' or old['gpu_uuid']!=gpu or old['binary_sha256']!=binary_fingerprints(build) or old['frozen']!=FROZEN:
        raise ValueError('D0 configuration/build/device differs; no D1')
    if json.loads((path/'result.json').read_text()).get('escaped_owners'):raise ValueError('D0 left owner processes; recovery unconfirmed')
    validate_run(path/'worker','group-bound-none',control_facts(path/'worker'))

def readiness_check(r,gpu):
    if not r['cuda_usable'] or not r['rm_device_accessible']:raise ValueError('DEVICE_ACCESS_BLOCKED')
    calls=[json.loads(s) for s in r['cuda_probe']['stdout'].splitlines() if s.startswith('{')]
    def values(name):return [x['detail'] for x in calls if x['call']==name and x['returncode']==0]
    if values('device_count')!=['1'] or values('gpu_uuid')!=[gpu[4:].replace('-','').lower()]:raise ValueError('CUDA GPU scope mismatch')
    numeric=lambda name:[x for x in values(name) if str(x).isdecimal()]
    if numeric('compute_capability_major')!=['8'] or numeric('compute_capability_minor')!=['0'] or numeric('multiprocessor_count')!=['108']:
        raise ValueError('Frozen Phase7 workload requires reviewed A100 / 108 SM scope')
    check_compute_processes(r,gpu)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--gpu',required=True);p.add_argument('--condition',choices=['D0','D1'],required=True)
    p.add_argument('--d0',type=Path);p.add_argument('--test-host-confirmed',action='store_true');a=p.parse_args()
    a.build=a.build.resolve();a.output=a.output.resolve()
    if not re.fullmatch(r'GPU-[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}',a.gpu):p.error('One full GPU UUID required')
    if not re.fullmatch(r'[A-Za-z0-9_.-]{1,100}',a.output.name):p.error('Output directory basename must be a short run ID')
    nsys=shutil.which('nsys')
    if not nsys:p.error('Nsight Systems is not installed; no automatic installation')
    cmd=profile_command(nsys,a.build,a.output,a.gpu,a.condition,a.test_host_confirmed)
    if a.condition=='D1':
        if not a.d0:p.error('D1 needs --d0 with an actually verified diagnostic backend')
        validate_d0(a.d0,a.build,a.gpu)
        # One active follow-up per D0, reserved before any subprocess. A failure
        # remains consumed; explicit additional experiments need a new review.
        with (a.d0/'d1_reserved.json').open('x') as f:json.dump({'output':str(a.output),'max_owner_attempts':1,'automatic_retry':False},f)
    a.output.mkdir(parents=True,exist_ok=False)
    os.environ['CUDA_VISIBLE_DEVICES']=a.gpu
    env=profiler_environment(os.environ,a.gpu) # inject only the application
    invocation={'run_kind':'diagnostic','condition':a.condition,'gpu_uuid':a.gpu,'frozen':FROZEN,
                'repo_commit':command(['git','rev-parse','HEAD'])['stdout'].strip(),
                'worktree':command(['git','status','--short'])['stdout'],
                'binary_sha256':binary_fingerprints(a.build),'command':cmd,'tool_version':command([nsys,'--version']),
                'active_authorized':a.test_host_confirmed,'automatic_retry':False,
                'profiler_environment_keys':sorted(env),
                'collection_scope':'system GPU context-switch records; derived analysis only this run owners; no deanonymization',
                'profiler_termination':'kill=none, duration=0, wait=all; owner cleanup must complete; outer failure is unconfirmed'}
    write(a.output/'invocation.json',invocation)
    project=Path(__file__).resolve().parents[1]
    sources=list((project/'src').glob('*'))+list((project/'scripts').glob('*.py'))+[project/'CMakeLists.txt']
    write(a.output/'source_fingerprints.json',{str(f.relative_to(project.parent)):hashlib.sha256(f.read_bytes()).hexdigest() for f in sources if f.is_file()})
    result={'run_kind':'diagnostic','condition':a.condition,'active_attempts':0,'state':'PREPARING'}
    try:
        ready=collect(a.build/'preflight_cuda');write(a.output/'preflight.json',ready);readiness_check(ready,a.gpu)
        result.update(state='COLLECTING',active_attempts=None if a.condition=='D1' else 0,
                      active_attempts_unknown=a.condition=='D1')
        write(a.output/'result.json',result) # interruption never fabricates zero attempts
        with (a.output/'collection.log').open('w') as log:
            execution=run_process_group(cmd,env,log,180)
        survivors=cleanup_escaped_owners(a.output/'worker');result['escaped_owners']=survivors
        if survivors:
            execution['termination'].append('ESCAPED_OWNERS_RECOVERY_UNCONFIRMED');execution['returncode']=125
            execution['recovery']='unconfirmed'
        write(a.output/'execution.json',execution)
        # Keep primary mmap evidence on interruption, independent of report export.
        worker=a.output/'worker'
        if worker.exists():recover_journals(worker,a.build,env)
        facts=control_facts(worker);write(a.output/'control_facts.json',facts);result['active_attempts']=facts['preempt']
        result['active_attempts_unknown']=facts['preempt_unknown'] or (a.condition=='D1' and facts['owners']!=['bg','int'])
        result['execution']=execution
        try:
            validate_run(worker,'group-bound-none' if a.condition=='D0' else 'group-preempt-wait',facts)
            result['worker_complete_correct_cleanup']=True
        except (OSError,ValueError,KeyError) as e:result['worker_complete_correct_cleanup']=False;result['worker_failure']=str(e)
        report=a.output/'trace.nsys-rep'
        if report.exists():
            for kind,suffix in [('sqlite','.sqlite'),('info','.info')]:
                export=command([nsys,'export','--type='+kind,'--force-overwrite=false','--output='+str(a.output/('trace'+suffix)),str(report)],60)
                write(a.output/('export_'+kind+'.json'),export)
        result['state']='CAPTURE_FINISHED_ANALYSIS_REQUIRED' if execution['returncode']==0 else 'CAPTURE_FAILED'
    except (OSError,ValueError,KeyError) as e:result['state']='STOPPED';result['reason']=str(e)
    finally:write(a.output/'result.json',result)
    print(json.dumps(result,indent=2))
    return 0 if result['state']=='CAPTURE_FINISHED_ANALYSIS_REQUIRED' else 1

if __name__=='__main__':raise SystemExit(main())
