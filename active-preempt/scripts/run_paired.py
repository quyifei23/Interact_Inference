#!/usr/bin/env python3
"""One frozen 10-pair batch. Fresh owner processes, no retries or resumed batches."""
import argparse
import datetime
import gzip
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
from pair_contract import (CONDITIONS,MAX_ATTEMPTS,check_plan,command_for,common_config,digest,empty_ledger,
                           make_schedule,recount,reserve,validate_configuration,write_json)
from run_matrix import binary_fingerprints,run_process_group
from summarize import ACCEPTED,read_rows,summarize
from preflight import command,collect

GET_INFO=0xa06c0106
PREEMPT=0xa06c0105

def utc():return datetime.datetime.now(datetime.timezone.utc).isoformat()

def environment(gpu):
    calls=[command(c) for c in (
        ['nvidia-smi','--query-compute-apps=pid,process_name,gpu_uuid','--format=csv,noheader'],
        ['nvidia-smi','-i',gpu,'--query-gpu=uuid,name,driver_version,pstate,temperature.gpu,clocks.sm,clocks.mem,utilization.gpu,utilization.memory','--format=csv,noheader'],
        ['cat','/proc/driver/nvidia/version'])]
    return dict(utc=utc(),gpu_uuid=gpu,commands=calls,cpu_affinity=sorted(os.sched_getaffinity(0)),
                visibility=os.environ.get('CUDA_VISIBLE_DEVICES'),mps='unknown; no reconfiguration',
                isolation='operator confirmation required; process snapshots are not proof')

def validate_environment(env,plan):
    if env['visibility']!=plan['gpu_uuid']:raise ValueError('CUDA_VISIBLE_DEVICES must equal the frozen full GPU UUID')
    if env['cpu_affinity']!=plan['frozen_configuration']['cpu_affinity']:raise ValueError('CPU affinity changed')
    proc,gpu,version=env['commands']
    if any(x.get('returncode')!=0 for x in (proc,gpu,version)):raise ValueError('Current GPU/process/driver visibility unavailable')
    for line in proc['stdout'].splitlines():
        fields=[f.strip() for f in line.split(',')]
        if len(fields)!=3 or not fields[2].startswith('GPU-'):raise ValueError('Unknown process-list format')
        if fields[2].lower()==plan['gpu_uuid'].lower():raise ValueError('Target GPU reports another compute task; stop')
    values=[v.strip() for v in gpu['stdout'].strip().split(',')]
    if len(values)!=9 or values[0].lower()!=plan['gpu_uuid'].lower() or values[2]!=plan['frozen_configuration']['build_profile']:
        raise ValueError('Current GPU/driver differs from frozen profile')
    if version['stdout']!=plan['frozen_configuration']['runtime_driver_text']:raise ValueError('KMD changed')

def validate_readiness(readiness,plan):
    if not readiness.get('cuda_usable') or not readiness.get('rm_device_accessible'):raise ValueError('DEVICE_ACCESS_BLOCKED')
    calls=[json.loads(s) for s in readiness['cuda_probe'].get('stdout','').splitlines() if s.startswith('{')]
    details=lambda name:[s['detail'] for s in calls if s['call']==name and s['returncode']==0]
    numbers=lambda name:[v for v in details(name) if isinstance(v,str) and v.isdecimal()]
    c=plan['frozen_configuration']
    if details('device_count')!=['1'] or details('gpu_uuid')!=[c['gpu_uuid']]:raise ValueError('CUDA device scope mismatch')
    for name,key in [('compute_capability_major','compute_major'),('compute_capability_minor','compute_minor'),('multiprocessor_count','sm_count')]:
        if numbers(name)!=[str(c[key])]:raise ValueError('CUDA workload GPU differs from frozen configuration')
    if c['compute_major']!=8 or c['compute_minor']!=0:raise ValueError('WORKLOAD_GPU_UNREVIEWED')

def control_facts(directory):
    """Unknown PREEMPT state consumes a slot; absence is not acceptance."""
    events=[];owners=[]
    for owner in ('bg','int'):
        path=directory/f'{owner}_control_events.jsonl'
        if path.is_file():
            owners.append(owner);events.extend(dict(e,recorded_owner=owner) for e in map(json.loads,path.read_text().splitlines()))
    rm=[e for e in events if e.get('event_kind')=='RM_CONTROL']
    attempted=[e for e in rm if e.get('attempted') is True]
    pending=[e for e in rm if e.get('operation_state')=='IN_FLIGHT']
    return dict(events=events,owners=owners,get_info=sum(e['command']==GET_INFO for e in attempted),
                preempt=sum(e['command']==PREEMPT for e in attempted),
                preempt_unknown=any(e.get('command')==PREEMPT for e in pending),
                other_project_controls=[e for e in attempted if e.get('command') not in (GET_INFO,PREEMPT)],
                pending=len(pending))

def validate_run(directory,mode,facts):
    rows=read_rows(directory/'raw.csv')
    if len(rows)!=1 or rows[0]['trial']!='0' or rows[0].get('local_trial_id')!='0':raise ValueError('Expected exactly one local trial 0')
    row=rows[0]
    if row.get('measurement_contract')!='group-preparation-v1' or row['mode']!=mode or row['graph']!='0':raise ValueError('Wrong measurement/mode/schema')
    if row['trial_state']!='complete' or row['correctness_status']!='PASS' or row['application_valid']!='1':raise ValueError('Incomplete/incorrect/unobserved trial; stop without replacement')
    if row.get('failure'):raise ValueError('Recorded trial failure: '+row['failure'])
    if facts['owners']!=['bg','int'] or facts['get_info']!=2 or facts['pending'] or facts['other_project_controls']:raise ValueError('Unexpected/missing project RM operations')
    events=facts['events']
    for owner in ('bg','int'):
        queries=[e for e in events if e.get('command')==GET_INFO and e['recorded_owner']==owner]
        if len(queries)!=1 or queries[0]['control_status']!=ACCEPTED:raise ValueError('Current group GET_INFO not verified')
        state=json.loads((directory/f'{owner}_profile_state.json').read_text())
        # Counters are separate from libcuda controls and local preparation.
        if state['project_group_get_info_attempted']!=1 or state['project_other_active_controls_attempted']!=0:raise ValueError('Unexpected owner counters')
    prep=[e for e in events if e.get('event_kind','').startswith('CONTROL_PREPARATION')]
    if len(prep)!=1 or prep[0]['recorded_owner']!='bg' or prep[0].get('attempted') is not False:raise ValueError('Common BG preparation absent/repeated')
    if mode=='group-bound-none':
        if facts['preempt'] or row['control_status']!='SKIPPED_BY_DESIGN' or any(row.get(k) for k in ('T_rm_call_begin','T_rm_call_end','rm_status')):raise ValueError('No-op issued/reported RM syscall')
        for owner in ('bg','int'):
            if json.loads((directory/f'{owner}_profile_state.json').read_text())['active_experiment_authorized']:raise ValueError('No-op became authorized')
    elif row['control_status']==ACCEPTED:
        preempts=[e for e in events if e.get('command')==PREEMPT and e.get('attempted')]
        if facts['preempt']!=1 or len(preempts)!=1 or preempts[0]['recorded_owner']!='bg' or preempts[0]['control_status']!=ACCEPTED:raise ValueError('PREEMPT owner/count/status mismatch')
    elif row['control_status']=='BG_ALREADY_COMPLETED_NO_PREEMPT':
        if facts['preempt']:raise ValueError('Already completed BG still preempted')
    else:raise ValueError('Control failed: '+row['control_status'])
    for owner in ('int','bg'):
        text=(directory/f'{owner}_cuda_cleanup.log').read_text()
        codes=re.findall(r'code=(-?\d+)',text)
        if 'cuCtxDestroy' not in text or not codes or any(int(c) for c in codes):raise ValueError('CUDA cleanup unverified')
        text=(directory/f'{owner}_recovery.txt').read_text()
        if 'CONFIGURATION_RESTORED' not in text or 'RECOVERY_FAILED' in text:raise ValueError('Owner recovery unverified')
    if not (directory/'status.txt').read_text().startswith('COMPLETED:') or not (directory/'bg_status.txt').read_text().startswith('COMPLETED'):raise ValueError('Owner completion unverified')
    reuse=json.loads((directory/'bg_context_usability.json').read_text())
    if not all(reuse.get(k) is True for k in ('same_context','same_stream','reference_match')):raise ValueError('BG reuse failed')
    return row

def plan_batch(args):
    if not re.fullmatch(r'[A-Za-z0-9_.-]+',args.batch_id) or args.batch_id in ('.','..'):raise ValueError('Invalid batch ID')
    prep=args.preparation.resolve();inv=json.loads((prep.parent/'invocation.json').read_text())
    actual=json.loads((prep/'configuration.json').read_text());facts=control_facts(prep)
    validate_run(prep,'group-bound-none',facts)
    build=args.build.resolve()
    if inv['binary_sha256']!=binary_fingerprints(build):raise ValueError('Build changed since non-active preparation')
    if not re.fullmatch(r'GPU-[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}',args.gpu):raise ValueError('Use one full GPU UUID')
    if args.gpu[4:].replace('-','').lower()!=actual['gpu_uuid']:raise ValueError('GPU differs from preparation')
    # Freeze after non-active preparation, before any new active sample. The
    # command line deliberately has no budget/iteration tuning switches.
    body=dict(schema_version=1,batch_id=args.batch_id,created_utc=utc(),seed=20260917,
              repo_commit=command(['git','rev-parse','HEAD'])['stdout'].strip(),
              gpu_uuid=args.gpu,build=str(build),binary_sha256=binary_fingerprints(build),
              frozen_configuration=common_config(actual),configuration_sha256=digest(common_config(actual)),
              max_preempt_attempts=MAX_ATTEMPTS,authorization='not_granted_by_plan_or_prior_evidence',
              preparation=str(prep),pairs=make_schedule(args.batch_id))
    body['plan_sha256']=digest(body);check_plan(body)
    args.output.mkdir(parents=True,exist_ok=False);write_json(args.output/'plan.json',body)
    write_json(args.output/'attempts.json',empty_ledger(body))
    (args.output/'source.diff').write_text(command(['git','diff','--','active-preempt'])['stdout'])
    print(args.output/'plan.json')

def recover_journals(directory,build,env):
    for path in directory.glob('*_control_events.bin'):
        with (directory/(path.stem+'_recovery.log')).open('w') as log:
            r=run_process_group([str(build/'event_dump'),str(path),str(path.with_suffix('.jsonl'))],env,log,15)
        if r['returncode']:raise ValueError('Journal recovery failed; retain original binary')
    # The mmap format is fixed-size; compress after recovery, outside the trial.
    # A round-trip verification precedes deletion of the uncompressed copy.
    for path in directory.glob('*_control_events.bin'):
        zipped=path.with_suffix(path.suffix+'.gz')
        with path.open('rb') as inp,gzip.open(zipped,'wb') as out:shutil.copyfileobj(inp,out)
        with gzip.open(zipped,'rb') as inp:
            if inp.read()!=path.read_bytes():raise ValueError('Journal compression verification failed')
        path.unlink()

def execute(args):
    path=args.plan.resolve();directory=path.parent;plan=json.loads(path.read_text());check_plan(plan)
    ledger=json.loads((directory/'attempts.json').read_text())
    if not args.test_host_confirmed:raise ValueError('Explicit NEW batch authorization required before execution')
    if ledger['execution_started'] or ledger['runs']:raise ValueError('Batch already started; retries/resumption are forbidden')
    # Exclusive marker is deliberately not removed, including on failure/crash.
    with (directory/'execution_started.json').open('x') as f:
        json.dump(dict(utc=utc(),gpu=plan['gpu_uuid'],plan_sha256=plan['plan_sha256'],max_preempt_attempts=MAX_ATTEMPTS,
                       operator_confirmation_flag=True,scope='this batch only; isolation confirmed by operator'),f,indent=2)
    ledger['execution_started']=True;write_json(directory/'attempts.json',ledger)
    env=os.environ.copy();build=Path(plan['build'])
    env['LD_PRELOAD']=str(build/'librm_control.so')
    def interrupted(signum,frame):raise InterruptedError("Batch interrupted; preserve reservation and stop")
    signal.signal(signal.SIGTERM,interrupted)
    current_record=None
    try:
        if os.environ.get('LD_PRELOAD'):raise ValueError('Unreviewed external LD_PRELOAD')
        readiness=collect(build/'preflight_cuda');write_json(directory/'preflight.json',readiness);validate_readiness(readiness,plan)
        for pair in plan['pairs']:
            for run in pair['runs']:
                current_record=None
                check_plan(json.loads(path.read_text()))
                if binary_fingerprints(build)!=plan['binary_sha256']:raise ValueError('Frozen binary changed')
                before=environment(plan['gpu_uuid'])
                write_json(directory/(run['run_id']+'-environment-before.json'),before);validate_environment(before,plan)
                out=directory/run['run_id'];out.mkdir(exist_ok=False)
                current_record=reserve(ledger,plan,pair,run);current_record['directory']=run['run_id'];current_record['reserved_utc']=utc()
                write_json(directory/'attempts.json',ledger) # reservation precedes any spawn
                cmd=command_for(build,out,plan,pair,run,args.test_host_confirmed)
                print(run['run_id'],run['condition'],'slot',ledger['active_slots_reserved'],flush=True)
                with (out/'process.log').open('w') as log:process=run_process_group(cmd,env,log,120)
                current_record['process']=process;write_json(directory/'attempts.json',ledger)
                recover_journals(out,build,env)
                facts=control_facts(out)
                current_record['get_info_attempts']=facts['get_info']
                current_record['preempt_attempts']=None if (facts['preempt_unknown'] or facts['owners']!=['bg','int']) and run['condition']=='treatment' else facts['preempt']
                current_record['other_project_controls']=len(facts['other_project_controls']);recount(ledger)
                write_json(directory/'attempts.json',ledger) # raw outcomes survive subsequent validation failure
                if (out/'raw.csv').exists():summarize(out)
                after=environment(plan['gpu_uuid']);write_json(out/'environment-after.json',after);validate_environment(after,plan)
                if process['returncode'] or process['timed_out'] or process['termination']:raise ValueError('Worker/process-tree failure; recovery must be inspected')
                current_record['configuration_sha256']=validate_configuration(json.loads((out/'configuration.json').read_text()),plan,pair,run)
                row=validate_run(out,run['mode'],facts)
                current_record.update(state='COMPLETE',cleanup_verified=True,control_status=row['control_status'],
                                      ordering=row['ordering_relative_to_rm'],setup_status=row['setup_status'],completed_utc=utc())
                write_json(directory/'attempts.json',ledger)
        ledger['completed']=True
    except BaseException as error:
        ledger['stopped_reason']=type(error).__name__+': '+str(error)
        if current_record and current_record['state']!='COMPLETE':current_record['state']='STOPPED_NO_RETRY'
        recount(ledger);write_json(directory/'attempts.json',ledger)
        print(ledger['stopped_reason'],file=sys.stderr)
    finally:
        write_json(directory/'attempts.json',ledger)
        from analyze_pairs import analyze
        analyze(directory)
    return 1 if ledger['stopped_reason'] else 0

def main():
    p=argparse.ArgumentParser(description=__doc__);sub=p.add_subparsers(dest='action',required=True)
    plan=sub.add_parser('plan');plan.add_argument('--build',type=Path,required=True);plan.add_argument('--preparation',type=Path,required=True)
    plan.add_argument('--gpu',required=True);plan.add_argument('--batch-id',required=True);plan.add_argument('--output',type=Path,required=True)
    run=sub.add_parser('execute');run.add_argument('--plan',type=Path,required=True);run.add_argument('--test-host-confirmed',action='store_true')
    a=p.parse_args()
    try:
        if a.action=='plan':plan_batch(a);return 0
        return execute(a)
    except (ValueError,OSError,KeyError) as e:p.exit(1,str(e)+'\n')
if __name__=='__main__':raise SystemExit(main())
