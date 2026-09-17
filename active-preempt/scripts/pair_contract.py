"""Small fixed-batch contract. No CUDA calls; synthetic callers use temp fixtures."""
import hashlib
import json
import random
from pathlib import Path

CONDITIONS={'control':'group-bound-none','treatment':'group-preempt-wait'}
MAX_ATTEMPTS=10
# Everything else in actual configuration participates, including additions in
# future workers. Do not whitelist a small convenient subset of workload fields.
VARYING={'mode','condition','batch_id','pair_id','pair_order','run_id'}

def digest(value):
    return hashlib.sha256(json.dumps(value,sort_keys=True,separators=(',',':')).encode()).hexdigest()

def write_json(path,value):
    path=Path(path);temporary=path.with_suffix(path.suffix+'.tmp')
    temporary.write_text(json.dumps(value,indent=2)+'\n');temporary.replace(path)

def common_config(configuration):
    return {k:v for k,v in configuration.items() if k not in VARYING}

def config_fingerprint(configuration):return digest(common_config(configuration))

def make_schedule(batch_id,seed=20260917):
    rng=random.Random(seed);orders=['CT']*5+['TC']*5;rng.shuffle(orders)
    pairs=[]
    for i,order in enumerate(orders):
        delay=rng.randint(1000,5000)
        pairs.append(dict(pair_id=i,pair_order=order,trigger_delay_us=delay,runs=[
            dict(condition='control' if letter=='C' else 'treatment',
                 mode=CONDITIONS['control' if letter=='C' else 'treatment'],
                 run_id=f'{batch_id}-p{i:02d}-{letter}',local_trial_id=0) for letter in order]))
    return pairs

def check_plan(plan):
    body={k:v for k,v in plan.items() if k!='plan_sha256'}
    if plan.get('plan_sha256')!=digest(body):raise ValueError('Plan changed after freeze')
    if plan.get('max_preempt_attempts')!=MAX_ATTEMPTS:raise ValueError('Unsupported active budget')
    if plan.get('pairs')!=make_schedule(plan['batch_id'],plan['seed']):raise ValueError('Schedule/order/delay/local trial differs from fixed plan')
    c=plan['frozen_configuration']
    for key,value in dict(schema_version=2,measurement_contract='group-preparation-v1',graph=0,force=0,bypass=0,
                          run_kind='performance',threads_per_block=256,dynamic_shared_bytes=65536,heartbeat_ns=2000,
                          bg_iterations=1630976,int_iterations=5888,cta_waves=1).items():
        if c.get(key)!=value:raise ValueError(f'Unreviewed fixed configuration {key}')
    if c.get('bg_cpu_affinity')!=c.get('cpu_affinity'):raise ValueError('Owner CPU affinity differs')
    if plan['configuration_sha256']!=digest(c):raise ValueError('Frozen configuration hash mismatch')
    if not plan['binary_sha256']:raise ValueError('Missing frozen build evidence')

def expected_config(plan,pair):
    c=dict(plan['frozen_configuration']);c['trigger_delay_us']=pair['trigger_delay_us'];return c

def validate_configuration(actual,plan,pair,run):
    expected=dict(batch_id=plan['batch_id'],pair_id=str(pair['pair_id']),pair_order=pair['pair_order'],
                  condition=run['condition'],run_id=run['run_id'],mode=run['mode'])
    if any(actual.get(k)!=v for k,v in expected.items()):raise ValueError('Run metadata differs from plan')
    if common_config(actual)!=expected_config(plan,pair):raise ValueError('Configuration fingerprint differs from frozen pair')
    return config_fingerprint(actual)

def empty_ledger(plan):
    return dict(plan_sha256=plan['plan_sha256'],execution_started=False,active_slots_reserved=0,
                known_preempt_attempts=0,unknown_preempt_slots=0,stopped_reason=None,runs=[])

def reserve(ledger,plan,pair,run):
    """Persist the returned reservation BEFORE spawning; never recycle a slot."""
    if ledger['plan_sha256']!=plan['plan_sha256']:raise ValueError('Ledger/plan mismatch')
    if ledger.get('stopped_reason'):raise ValueError('Stopped batch cannot resume')
    flat=[(p,r) for p in plan['pairs'] for r in p['runs']]
    index=len(ledger['runs'])
    if index>=len(flat) or (pair,run)!=flat[index]:raise ValueError('Out-of-order/repeated/unplanned run')
    if index and ledger['runs'][-1].get('state')!='COMPLETE':raise ValueError('Previous run incomplete/unknown; no retry or continuation')
    active=run['condition']=='treatment'
    if active and ledger['active_slots_reserved']>=MAX_ATTEMPTS:raise ValueError('Active attempt budget exhausted')
    if active:ledger['active_slots_reserved']+=1
    record=dict(**run,pair_id=pair['pair_id'],pair_order=pair['pair_order'],state='RESERVED_OUTCOME_UNKNOWN',
                preempt_attempts=None if active else 0,slot_consumed=active,cleanup_verified=False)
    ledger['runs'].append(record);recount(ledger);return record

def recount(ledger):
    ledger['known_preempt_attempts']=sum(r['preempt_attempts'] for r in ledger['runs'] if r.get('preempt_attempts') is not None)
    ledger['unknown_preempt_slots']=sum(r['slot_consumed'] and r.get('preempt_attempts') is None for r in ledger['runs'])
    if ledger['known_preempt_attempts']+ledger['unknown_preempt_slots']>MAX_ATTEMPTS:raise ValueError('Active ceiling violated')

def command_for(build,directory,plan,pair,run,authorized):
    active=run['condition']=='treatment'
    if active and not authorized:raise ValueError('New batch authorization required; historical evidence grants none')
    c=plan['frozen_configuration']
    cmd=[str(Path(build)/'int_worker'),'--run-dir',str(directory),'--mode',run['mode'],'--trials','1']
    for key in ('bg_iterations','int_iterations','heartbeat_ns','cta_waves'):
        cmd+=['--'+key.replace('_','-'),str(c[key])]
    for key,value in dict(trigger_delay_us=pair['trigger_delay_us'],batch_id=plan['batch_id'],pair_id=pair['pair_id'],
                          pair_order=pair['pair_order'],condition=run['condition'],run_id=run['run_id']).items():
        cmd+=['--'+key.replace('_','-'),str(value)]
    if active:cmd.append('--test-host-confirmed')
    return cmd
