#!/usr/bin/env python3
"""Schema 2 only. Application latency, ordering, correctness and control state stay independent."""
import argparse
from collections import Counter
import csv
from fractions import Fraction
import json
from pathlib import Path

SCHEMA_VERSION = '2'
ACCEPTED = 'CONTROL_ACCEPTED_EFFECT_UNVERIFIED'

def quantiles(values):
    if not values: return None
    values = sorted(values)
    def q(p):
        i=(len(values)-1)*p; a=int(i)
        return values[a]+(values[min(a+1,len(values)-1)]-values[a])*(i-a)
    return [q(.5),q(.95),q(.99),values[-1]]

def number(row,key):
    value=row.get(key,'')
    return int(value) if value not in ('',None,'unknown','null') else None

def delta(row,end,begin):
    e,b=number(row,end),number(row,begin)
    return (e-b)/1000 if e is not None and b is not None and e>=b else None

class CalibrationModel:
    """Optional affine offset assumption, NOT a measured hard error guarantee."""
    def __init__(self,before,after,margin_ns):
        if margin_ns<0: raise ValueError('negative calibration margin')
        def endpoint(path):
            with Path(path).open() as f: rows=list(csv.DictReader(f))
            if not rows: raise ValueError('empty calibration')
            if any(int(r['cpu_observed_ns'])<int(r['cpu_send_ns']) for r in rows): raise ValueError('invalid calibration bracket')
            r=min(rows,key=lambda r:int(r['cpu_observed_ns'])-int(r['cpu_send_ns']))
            g=int(r['gpu_ns'])
            return g,int(r['cpu_send_ns'])-g,int(r['cpu_observed_ns'])-g
        self.a,self.b=endpoint(before),endpoint(after);self.margin=margin_ns
        if self.a[0]>=self.b[0]: raise ValueError('calibration clock order invalid')
    def interval(self,gpu_ns):
        if not self.a[0]<=gpu_ns<=self.b[0]: return None
        w=Fraction(gpu_ns-self.a[0],self.b[0]-self.a[0])
        low=Fraction(gpu_ns)+self.a[1]*(1-w)+self.b[1]*w-self.margin
        high=Fraction(gpu_ns)+self.a[2]*(1-w)+self.b[2]*w+self.margin
        return low,high

def ordering(row,model=None):
    rm=number(row,'T_rm_call_begin');observed=number(row,'T_int_graph_entry_observed')
    if rm is None:return 'ordering_ambiguous' if row.get('control_status')=='CONTROL_RESULT_UNAVAILABLE' else 'not_applicable'
    if observed is not None and observed<rm:return 'before_rm'
    gpu=number(row,'T_int_graph_entry_gpu_ns')
    if model is not None and gpu is not None:
        interval=model.interval(gpu)
        if interval and interval[0]>rm:return 'after_rm_under_calibration_model'
    return 'ordering_ambiguous'

def read_rows(path):
    with Path(path).open() as f:
        reader=csv.DictReader(f); rows=list(reader);fields=reader.fieldnames or []
    if not rows: raise ValueError('No measured rows; refusing synthetic summary')
    required=set('schema_version trial mode run_kind graph trial_state T_cpu_trigger T_int_submit_begin T_int_submit_end T_ipc_send T_ipc_received T_ipc_ack T_rm_call_begin T_rm_call_end rm_syscall_result rm_status T_int_graph_entry_observed T_int_main_entry_observed T_int_graph_done_observed T_int_graph_entry_gpu_ns T_int_graph_done_gpu_ns bg_done_before_interaction bg_done_before_control_observed control_target_running application_valid control_status gpu_overlap correctness_status'.split())
    if len(set(fields))!=len(fields) or not required<=set(fields):raise ValueError('Missing/duplicate schema 2 columns; refuse ambiguous field semantics')
    if any(None in r or any(v is None for v in r.values()) for r in rows):raise ValueError('Malformed CSV field count')
    if {r.get('schema_version') for r in rows}!={SCHEMA_VERSION}:raise ValueError('Schema 2 required; legacy schema 1 has main-entry semantics and must not be mixed')
    if len({r['run_kind'] for r in rows})!=1:raise ValueError('Diagnostic and performance samples must not be mixed')
    return rows

def summarize(directory,calibration_margin_ns=None):
    directory=Path(directory);rows=read_rows(directory/'raw.csv');model=None
    if calibration_margin_ns is not None:
        model=CalibrationModel(directory/'int_calibration_before.csv',directory/'int_calibration_after.csv',calibration_margin_ns)
    for r in rows:
        if r['control_status']==ACCEPTED and (r['rm_syscall_result']!='0' or r['rm_status']!='0'):
            r['control_status']='INCONSISTENT_CONTROL_RECORD'
        r['derived_ordering']=ordering(r,model)
    # Do not filter RM errors, before-RM, late BG, or ambiguous samples out of
    # the application distribution. Correctness is reported as another dimension.
    valid=[r for r in rows if r['application_valid']=='1' and delta(r,'T_int_graph_entry_observed','T_cpu_trigger') is not None]
    correct=[r for r in valid if r['correctness_status']=='PASS']
    subset=[r for r in correct if r['control_status']==ACCEPTED and r['derived_ordering']=='after_rm_under_calibration_model'
            and r['gpu_overlap']=='lifetime_overlap' and r['bg_done_before_interaction']=='0'
            and r['bg_done_before_control_observed']=='0' and r.get('bg_done_at_owner_check')!='1' and r['trial_state']=='complete']
    dimensions={k:dict(Counter(r[k] for r in rows)) for k in ('trial_state','application_valid','control_status','gpu_overlap','derived_ordering','correctness_status','control_target_running','bg_done_before_interaction','bg_done_before_control_observed')}
    for key in ('bg_done_at_owner_check','setup_status'):
        if any(key in r for r in rows):dimensions[key]=dict(Counter(r.get(key,'unknown') for r in rows))
    stats={'schema_version':2,'rows':len(rows),'application_valid':len(valid),'correct_application_rows':len(correct),'timing_eligible_subset':len(subset),
           'preemption_confirmed':0,'dimensions':dimensions,'calibration_model':None if model is None else {
               'name':'affine offset between minimum-RTT endpoint brackets plus explicit margin','margin_ns':calibration_margin_ns,
               'status':'assumption; empirical brackets are not a whole-run error guarantee'}}
    stats['timeout_rows']=sum('timeout' in r.get('failure','').lower() or 'deadline' in r.get('failure','').lower() or r['control_status']=='CONTROL_TIMEOUT' for r in rows)
    stats['missing_entry_or_done_observation']=sum(not r['T_int_graph_entry_observed'] or not r['T_int_graph_done_observed'] for r in rows)
    stats['failure_messages']=dict(Counter(r.get('failure','') for r in rows if r.get('failure','')))
    lines=['# Schema 2 measurement summary','',f"Run kind: {rows[0]['run_kind']}. Rows: {len(rows)}; valid application observations: {len(valid)}; timing-eligible subset: {len(subset)}.",'',
           'All valid application observations include RM failures and ambiguous/before-RM ordering. Correctness and control outcomes are reported independently. No hardware preemption is confirmed by this summary.','']
    for key,value in dimensions.items():lines.append(f'- {key}: {value}')
    lines+=[f"- timeout_rows: {stats['timeout_rows']}",f"- missing_entry_or_done_observation: {stats['missing_entry_or_done_observation']}",f"- failure_messages: {stats['failure_messages']}"]
    single=len(rows)==1
    lines+=['','One-trial smoke: raw values only; no distribution or performance ranking.','',
            '| Metric (μs) | n | raw value |','|---|---:|---:|'] if single else [
            '','| Metric (μs) | n | p50 | p95 | p99 | max |','|---|---:|---:|---:|---:|---:|']
    metrics=[('Interaction → graph entry observed, all application-valid',valid,'T_int_graph_entry_observed','T_cpu_trigger'),
             ('Interaction → graph entry, correct application rows',correct,'T_int_graph_entry_observed','T_cpu_trigger'),
             ('Interaction → graph entry, timing-eligible subset',subset,'T_int_graph_entry_observed','T_cpu_trigger'),
             ('Interaction → main entry observed',valid,'T_int_main_entry_observed','T_cpu_trigger'),
             ('Host submission',valid,'T_int_submit_end','T_int_submit_begin'),
             ('IPC request → owner receipt',rows,'T_ipc_received','T_ipc_send'),
             ('IPC round trip (includes owner control when present)',rows,'T_ipc_ack','T_ipc_send'),
             ('RM syscall wall time (including failures)',rows,'T_rm_call_end','T_rm_call_begin'),
             ('GPU graph marker interval',valid,'T_int_graph_done_gpu_ns','T_int_graph_entry_gpu_ns')]
    for title,source,end,begin in metrics:
        values=[v for r in source if (v:=delta(r,end,begin)) is not None];q=quantiles(values)
        if single:
            stats.setdefault('raw_values_us',{})[title]=values[0] if values else None
            rendered=f'{values[0]:.3f}' if values else 'N/A'
        else:rendered=' | '.join(f'{x:.3f}' for x in q) if q else 'N/A | N/A | N/A | N/A'
        lines.append(f'| {title} | {len(values)} | {rendered} |')
    lines+=['','Exact BG preempt completion, context-save duration, and BG resume latency: **unmeasured**.',
            '', 'Graph entry is K0 block0/thread0; main entry is the main arithmetic node. Neither is an exact first-warp timestamp. Lifetime overlap and an after-RM ordering hypothesis do not establish causation. Target residency stays unknown without another backend.',
            '', 'Ordering model: '+('none; observations at/after RM are ambiguous' if model is None else f'explicit affine-offset assumption with {calibration_margin_ns} ns margin; not a guaranteed bound'),
            '', 'Paired comparisons: group-preempt-wait / preempt-wait vs none; realtime-restart vs realtime-only. Compare identical fixed work/configurations and separate diagnostic runs. One pair cannot establish a causal or stable latency benefit.']
    for path in sorted(directory.glob('*_calibration_*.csv')):
        with path.open() as f: samples=list(csv.DictReader(f))
        rtt=[(int(s['cpu_observed_ns'])-int(s['cpu_send_ns']))/1000 for s in samples]
        description=f'ping samples={len(rtt)}, min/max RTT μs {min(rtt) if rtt else None}/{max(rtt) if rtt else None}' if single else f'ping RTT p50/p95/p99/max μs {quantiles(rtt)}'
        lines+=['',f'{path.name}: {description}. Minimum RTT is not a full-run hard error bound.']
    (directory/'analysis.json').write_text(json.dumps(stats,indent=2)+'\n')
    output=directory/'summary.md';output.write_text('\n'.join(lines)+'\n');return output

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',type=Path)
    p.add_argument('--calibration-margin-ns',type=int,help='Opt in to an explicitly assumed affine clock model; this margin is not an observed guarantee')
    a=p.parse_args();print(summarize(a.directory,a.calibration_margin_ns))
