#!/usr/bin/env python3
"""Staged CUDA/RM experiments. Never install drivers, grant rights, reset GPUs, or infer causation."""
import argparse
import datetime
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
from summarize import read_rows,summarize,ACCEPTED

BASELINES={'int-only','none'}
MODES=BASELINES|{'timeslice','preempt-wait','preempt-async','realtime-only','realtime-restart','disable','disable-split'}
EXTENDED={'preempt-async','disable','disable-split'}

def canonical(mode):return 'realtime-restart' if mode=='realtime' else mode

def run_process_group(cmd,env,log,timeout,grace=8):
    """Own session; reap/terminate its entire group on timeout or orphaned children."""
    try:process=subprocess.Popen(cmd,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
    except OSError as e:
        return dict(command=cmd,returncode=127,spawn_errno=e.errno,error=str(e),timed_out=False,
                    termination=[],recovery='not_started')
    actions=[];timed_out=False
    try:code=process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out=True;code=124;actions.append('SIGTERM_PROCESS_GROUP_FOR_OWNER_CLEANUP')
        try:os.killpg(process.pid,signal.SIGTERM)
        except ProcessLookupError:pass
        try:process.wait(timeout=grace)
        except subprocess.TimeoutExpired:pass
    def exists():
        try:os.killpg(process.pid,0);return True
        except ProcessLookupError:return False
    orphaned=exists()
    if orphaned:
        actions.append('DESCENDANTS_REMAIN_OR_RECOVERY_TIMEOUT')
        try:os.killpg(process.pid,signal.SIGTERM)
        except ProcessLookupError:pass
        end=time.monotonic()+grace
        while exists() and time.monotonic()<end:time.sleep(.05)
        if exists():
            actions.append('SIGKILL_PROCESS_GROUP_RECOVERY_UNCONFIRMED')
            try:os.killpg(process.pid,signal.SIGKILL)
            except ProcessLookupError:pass
        try:process.wait(timeout=2)
        except subprocess.TimeoutExpired:actions.append('PROCESS_REAP_TIMEOUT')
    return dict(command=cmd,returncode=code if code else (125 if orphaned else 0),timed_out=timed_out,
                termination=actions,recovery='unconfirmed' if actions else 'inspect_owner_recovery_logs')

def validate_smoke(evidence,mode,minimum,run_kind,graph=False,args=None):
    if not evidence or not (evidence/'status.txt').is_file():raise ValueError('A completed matching smoke result is required')
    if not (evidence/'status.txt').read_text().startswith('COMPLETED:'):raise ValueError('Smoke status is not COMPLETED')
    configuration=json.loads((evidence/'configuration.json').read_text())
    expected=dict(schema_version=2,mode=mode,graph=int(graph),run_kind=run_kind)
    if args is not None:
        expected.update({key:getattr(args,key) for key in ('force','bypass','cta_waves','heartbeat_ns','timeslice_us')})
    if any(configuration.get(key)!=value for key,value in expected.items()):raise ValueError('Smoke scheduling/workload configuration mismatch')
    if args is not None:
        for role in ('int','bg') if mode!='int-only' else ('int',):
            key=role+'_iterations';fixed=configuration.get(key)
            if not isinstance(fixed,int) or fixed<=0 or fixed%256 or fixed>1000000000:raise ValueError('Smoke fixed iteration count missing/invalid')
            requested=getattr(args,key)
            if requested and requested!=fixed:raise ValueError('Smoke fixed iteration count mismatch')
            # A 1 -> 10 progression reuses validated work, never recalibrates it.
            setattr(args,key,fixed)
    rows=read_rows(evidence/'raw.csv')
    if len(rows)<minimum or any(r['mode']!=mode or r['run_kind']!=run_kind or r['graph']!=str(int(graph))
         or r['trial_state']!='complete' or r['application_valid']!='1' or r['correctness_status']!='PASS'
         or (mode not in BASELINES|{'timeslice','realtime-only'} and r['control_status']!=ACCEPTED) for r in rows):
        raise ValueError('Smoke evidence configuration/completeness/observation/correctness mismatch')
    for role in ('int','bg') if mode!='int-only' else ('int',):
        f=evidence/f'{role}_recovery.txt'
        if not f.is_file() or 'RECOVERY_FAILED' in f.read_text() or 'CONFIGURATION_RESTORED' not in f.read_text():raise ValueError('Owner restoration evidence missing/failed')
    return rows

def validate_admission(args):
    args.modes=[canonical(m) for m in args.modes]
    if not set(args.modes)<=MODES:raise ValueError('unknown mode')
    if not 1<=args.trials<=100000:raise ValueError('trials must be 1..100000')
    if set(args.modes)-BASELINES and not args.test_host_confirmed:raise ValueError('Active modes require --test-host-confirmed; no privilege changes are performed')
    if (set(args.modes)&EXTENDED or args.force or args.bypass) and not args.allow_extended:raise ValueError('Async/force/bypass/D need --allow-extended after synchronous/recovery validation')
    kind='diagnostic' if args.diagnostic_progress else 'performance'
    if args.trials>10 and (not args.bg_iterations and args.modes[0]!='int-only' or not args.int_iterations):
        raise ValueError('Statistics require explicit fixed iterations copied from validated smoke configurations')
    if args.trials>1:
        if len(args.modes)!=1:raise ValueError('Beyond one-trial smoke, run one mode at a time with its matching --smoke-evidence')
        validate_smoke(args.smoke_evidence,args.modes[0],10 if args.trials>10 else 1,kind,args.graph,args)
    if args.graph:
        if not args.graph_evidence_reviewed:raise ValueError('Graph requires --graph-evidence-reviewed; acceptance/overlap alone cannot establish preemption')
        if not args.primitive_evidence:raise ValueError('Graph also requires --primitive-evidence from a plain-kernel active mode')
        rows=read_rows(args.primitive_evidence/'raw.csv')
        if not rows or rows[0]['mode'] not in {'preempt-wait','preempt-async','realtime-restart','disable','disable-split'}:raise ValueError('Need plain active-primitive evidence')
        validate_smoke(args.primitive_evidence,rows[0]['mode'],10,kind,False,args)
        if not (set(args.modes)-BASELINES-{'timeslice'})<={rows[0]['mode']}:raise ValueError('Graph active mode must match primitive evidence')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build',type=Path,default=Path(__file__).resolve().parents[1]/'build');p.add_argument('--output',type=Path,required=True)
    p.add_argument('--trials',type=int,default=1);p.add_argument('--modes',nargs='+',default=['int-only','none'])
    p.add_argument('--cta-waves',type=int,default=1);p.add_argument('--heartbeat-ns',type=int,default=2000)
    p.add_argument('--bg-iterations',type=int,default=0);p.add_argument('--int-iterations',type=int,default=0)
    p.add_argument('--force',type=int,choices=[0,1],default=0);p.add_argument('--bypass',type=int,choices=[0,1],default=0)
    p.add_argument('--timeslice-us',type=int,default=1)
    for flag in ('graph','diagnostic-progress','test-host-confirmed','allow-extended','graph-evidence-reviewed'):p.add_argument('--'+flag,action='store_true')
    p.add_argument('--smoke-evidence',type=Path);p.add_argument('--primitive-evidence',type=Path)
    a=p.parse_args()
    try:validate_admission(a)
    except (ValueError,OSError,KeyError) as e:p.error(str(e))
    a.output.mkdir(parents=True,exist_ok=False);a.output=a.output.resolve();a.build=a.build.resolve()
    (a.output/'invocation.json').write_text(json.dumps({'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'argv':sys.argv,
        'canonical_modes':a.modes,'run_kind':'diagnostic' if a.diagnostic_progress else 'performance'},indent=2)+'\n')
    env=os.environ.copy();env['LD_PRELOAD']=str(a.build/'librm_control.so')+(':'+env['LD_PRELOAD'] if env.get('LD_PRELOAD') else '')
    attempts=[]
    def run(name,cmd,timeout):
        out=a.output/name;out.mkdir(exist_ok=False)
        with (out/'process.log').open('w') as log:r=run_process_group(cmd,out_env,log,timeout)
        r['configuration']=name;attempts.append(r);(a.output/'attempts.json').write_text(json.dumps(attempts,indent=2)+'\n')
        for binary in out.glob('*_control_events.bin'):
            with (out/(binary.stem+'_recovery.log')).open('w') as log:
                recovered=run_process_group([str(a.build/'event_dump'),str(binary),str(binary.with_suffix('.jsonl'))],out_env,log,15)
            r.setdefault('journal_recovery',[]).append(recovered)
            if recovered['returncode']:r['returncode']=r['returncode'] or 126
        (a.output/'attempts.json').write_text(json.dumps(attempts,indent=2)+'\n')
        return r
    # The preflight script runs CUDA independently of the RM ABI profile. It
    # saves raw calls and errno even if nvidia-smi and CUDA fail differently.
    out_env=os.environ.copy()
    with (a.output/'preflight.log').open('w') as log:
        probe=run_process_group([sys.executable,str(Path(__file__).with_name('preflight.py')),'--binary',str(a.build/'preflight_cuda'),'--output',str(a.output/'preflight')],out_env,log,45)
    attempts.append(dict(configuration='probe-cuda',**probe));(a.output/'attempts.json').write_text(json.dumps(attempts,indent=2)+'\n')
    if probe['returncode']:
        (a.output/'summary.md').write_text('CUDA readiness failed. GPU trials = 0; no raw GPU samples generated. See preflight/preflight.json.\n');return probe['returncode']
    out_env=env
    if set(a.modes)-BASELINES:
        readiness=json.loads((a.output/'preflight/preflight.json').read_text())
        processes=next((r for r in readiness['commands'] if r['call'][0]=='nvidia-smi' and '--query-compute-apps=pid,process_name,gpu_uuid' in r['call']),None)
        if processes and processes.get('returncode')==0 and processes.get('stdout','').strip():
            (a.output/'summary.md').write_text('Active tests skipped: other GPU compute processes reported. GPU trials = 0.\n');return 77
        for stage in ('observe','identity','readonly'):
            name='probe-rm-'+stage;out=a.output/name
            r=run(name,[str(a.build/'int_worker'),'--probe-rm-'+stage,'--run-dir',str(out)],45)
            if r['returncode']:
                (a.output/'summary.md').write_text(f'{name} failed/skipped: exit {r["returncode"]}. Scheduling trials = 0.\n');return r['returncode']
    bg_iterations=a.bg_iterations;int_iterations=a.int_iterations
    for mode in a.modes:
        name=f'{mode}-f{a.force}-b{a.bypass}';out=a.output/name
        cmd=[str(a.build/'int_worker'),'--run-dir',str(out),'--mode',mode,'--trials',str(a.trials),'--cta-waves',str(a.cta_waves),
             '--heartbeat-ns',str(a.heartbeat_ns),'--timeslice-us',str(a.timeslice_us),'--force',str(a.force),'--bypass',str(a.bypass)]
        for flag in ('graph','diagnostic_progress','test_host_confirmed','allow_extended','graph_evidence_reviewed'):
            if getattr(a,flag):cmd.append('--'+flag.replace('_','-'))
        for key,value in [('bg-iterations',bg_iterations),('int-iterations',int_iterations)]:
            if value:cmd+=['--'+key,str(value)]
        r=run(name,cmd,max(120,a.trials*.5+120));print(name,'exit',r['returncode'],flush=True)
        if (out/'raw.csv').is_file():
            try:summarize(out)
            except ValueError as e:(out/'analysis_failure.txt').write_text(str(e)+'\n')
        if r['returncode']:
            (a.output/'summary.md').write_text(f'Stopped at {name}, exit {r["returncode"]}. Preserve journals/incomplete trials; inspect owner recovery. No following configuration executed.\n');return r['returncode']
        # Freeze calibrated work for subsequent paired configurations in this
        # one-trial matrix. Statistics additionally require explicit iterations.
        for role in ('int','bg'):
            path=out/f'{role}_identity.txt'
            if path.exists():
                match=re.search(r'^iterations=(\d+)',path.read_text(),re.M)
                if match:
                    if role=='int':int_iterations=int(match[1])
                    else:bg_iterations=int(match[1])
    (a.output/'summary.md').write_text('Requested runs completed. Compare matched controls and schema 2 dimensions. No automatic performance winner or preemption confirmation.\n')
    return 0

if __name__=='__main__':raise SystemExit(main())
