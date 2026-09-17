#!/usr/bin/env python3
"""Describe a frozen small paired experiment; integer differences, no tail SLA."""
import argparse
from collections import Counter
import csv
import json
from pathlib import Path
import statistics
from pair_contract import check_plan,expected_config,common_config,write_json
from summarize import read_rows,number,ordering,ACCEPTED

def difference_ns(row,end,begin):
    # Never convert absolute nanosecond timestamps to double before subtraction.
    e,b=number(row,end),number(row,begin)
    return e-b if e is not None and b is not None and e>=b else None

def describe(values):
    if not values:return dict(n=0,values_ns=[],median_ns=None,mean_ns=None,min_ns=None,max_ns=None)
    return dict(n=len(values),values_ns=values,median_ns=statistics.median(values),
                mean_ns=statistics.mean(values),min_ns=min(values),max_ns=max(values))

def paired_differences(pairs,metric):
    # Positive means treatment latency was lower. Keep adverse and equal values.
    return [p[f'control_{metric}_ns']-p[f'treatment_{metric}_ns'] for p in pairs
            if p['configuration_match'] and p.get(f'control_{metric}_ns') is not None and p.get(f'treatment_{metric}_ns') is not None]

def us(value):return 'unknown' if value is None else f'{value/1000:.3f}'

def analyze(directory):
    directory=Path(directory);plan=json.loads((directory/'plan.json').read_text());check_plan(plan)
    ledger=json.loads((directory/'attempts.json').read_text())
    attempted={r['run_id']:r for r in ledger['runs']};observations=[];pairs=[]
    if not attempted:
        (directory/'paired_summary.md').write_text('Plan frozen; no batch worker has been started. GPU paired trials = 0. No latency results generated.\n')
        return
    for pair in plan['pairs']:
        p=dict(pair_id=pair['pair_id'],pair_order=pair['pair_order'],planned_delay_us=pair['trigger_delay_us'],configuration_match=True)
        for run in pair['runs']:
            cond=run['condition'];record=attempted.get(run['run_id'],{});out=directory/run['run_id']
            row={};issue=None
            if (out/'raw.csv').is_file():
                try:
                    rows=read_rows(out/'raw.csv')
                    if len(rows)!=1:raise ValueError('expected one local trial')
                    row=rows[0]
                except ValueError as e:issue=str(e)
            configuration=None
            if (out/'configuration.json').is_file():configuration=json.loads((out/'configuration.json').read_text())
            match=configuration is not None and common_config(configuration)==expected_config(plan,pair)
            if not match:p['configuration_match']=False
            item=dict(run_id=run['run_id'],condition=cond,pair_id=pair['pair_id'],pair_order=pair['pair_order'],
                      run_state=record.get('state','NOT_STARTED'),configuration_match=match,analysis_error=issue,
                      entry_ns=difference_ns(row,'T_int_graph_entry_observed','T_cpu_trigger'),
                      done_ns=difference_ns(row,'T_int_graph_done_observed','T_cpu_trigger'),
                      application_valid=row.get('application_valid','unknown'),trial_state=row.get('trial_state','unknown'),
                      control_status=row.get('control_status','unknown'),ordering_relative_to_rm=ordering(row),
                      gpu_overlap=row.get('gpu_overlap','unknown'),correctness_status=row.get('correctness_status','unknown'),
                      cleanup_verified=record.get('cleanup_verified',False),failure=row.get('failure',''),
                      setup_status=row.get('setup_status','unknown'),bg_done_before_interaction=row.get('bg_done_before_interaction','unknown'),
                      bg_done_at_owner_check=row.get('bg_done_at_owner_check','unknown'),
                      trigger_lateness_ns=number(row,'trigger_lateness_ns'),actual_delay_ns=number(row,'actual_delay_ns'),
                      rm_wall_ns=difference_ns(row,'T_rm_call_end','T_rm_call_begin'),
                      prepare_ns=difference_ns(row,'T_owner_prepare_end','T_owner_prepare_begin'),
                      receive_to_prepare_ns=difference_ns(row,'T_owner_prepare_begin','T_owner_received'),
                      prepare_to_rm_ns=difference_ns(row,'T_rm_call_begin','T_owner_prepare_end'),
                      prepare_to_action_end_ns=difference_ns(row,'T_owner_action_end','T_owner_prepare_end'),
                      ipc_request_ns=difference_ns(row,'T_owner_received','T_ipc_send'),
                      ipc_roundtrip_ns=difference_ns(row,'T_ipc_ack','T_ipc_send'),
                      submit_ns=difference_ns(row,'T_int_submit_end','T_int_submit_begin'))
            observations.append(item)
            p.update({cond+'_'+k:v for k,v in item.items() if k not in ('condition','pair_id','pair_order')})
        p['complete_correct_pair']=p['configuration_match'] and all(p[c+'_correctness_status']=='PASS' and p[c+'_run_state']=='COMPLETE' for c in ('control','treatment'))
        for metric in ('entry','done'):
            c,t=p.get(f'control_{metric}_ns'),p.get(f'treatment_{metric}_ns')
            p['delta_'+metric+'_ns']=c-t if p['configuration_match'] and c is not None and t is not None else None
        pairs.append(p)
    if any(o['entry_ns'] is not None or o['done_ns'] is not None for o in observations):
        with (directory/'pairs.csv').open('w') as f:
            writer=csv.DictWriter(f,fieldnames=list(pairs[0]));writer.writeheader();writer.writerows(pairs)
    metrics={}
    for metric in ('entry','done'):
        deltas=paired_differences(pairs,metric)
        metrics[metric]=dict(control=describe([o[metric+'_ns'] for o in observations if o['condition']=='control' and o[metric+'_ns'] is not None]),
                            treatment=describe([o[metric+'_ns'] for o in observations if o['condition']=='treatment' and o[metric+'_ns'] is not None]),
                            delta=describe(deltas),improved=sum(d>0 for d in deltas),worse=sum(d<0 for d in deltas),equal=sum(d==0 for d in deltas),
                            by_order={order:describe(paired_differences([p for p in pairs if p['pair_order']==order],metric)) for order in ('CT','TC')},
                            complete_correct_subset=describe(paired_differences([p for p in pairs if p['complete_correct_pair']],metric)))
    dimensions={c:{key:dict(Counter(str(o[key]) for o in observations if o['condition']==c)) for key in
                     ('run_state','application_valid','trial_state','control_status','ordering_relative_to_rm','gpu_overlap','correctness_status','setup_status','cleanup_verified')}
                for c in ('control','treatment')}
    started=[o for o in observations if o['run_id'] in attempted]
    summary=dict(planned_pairs=10,planned_runs=20,started_runs=len(attempted),started_pairs=len({r['pair_id'] for r in ledger['runs']}),
                 missing_entry_started_runs=sum(o['entry_ns'] is None for o in started),missing_done_started_runs=sum(o['done_ns'] is None for o in started),
                 incomplete_trial_started_runs=sum(o['trial_state']!='complete' for o in started),
                 failed_or_unconfirmed_runs=sum(o['run_state']!='COMPLETE' for o in started),
                 control_error_rows=sum(o['control_status'] not in (ACCEPTED,'SKIPPED_BY_DESIGN','BG_ALREADY_COMPLETED_NO_PREEMPT','unknown') for o in started),
                 timeout_runs=sum('timeout' in o['failure'].lower() or o['control_status']=='CONTROL_TIMEOUT' or
                                  attempted[o['run_id']].get('process',{}).get('timed_out',False) for o in started),
                 mechanism_timing_eligible_samples=sum(o['condition']=='treatment' and o['application_valid']=='1' and o['correctness_status']=='PASS' and
                     o['control_status']==ACCEPTED and o['ordering_relative_to_rm']=='after_rm_under_calibration_model' and o['gpu_overlap']=='lifetime_overlap' and
                     o['bg_done_before_interaction']=='0' and o['bg_done_at_owner_check']=='0' for o in started),
                 complete_runs=sum(o['run_state']=='COMPLETE' for o in observations),
                 complete_pairs=sum(all(p[c+'_run_state']=='COMPLETE' for c in ('control','treatment')) for p in pairs),
                 active_slots_reserved=ledger['active_slots_reserved'],known_preempt_attempts=ledger['known_preempt_attempts'],
                 unknown_preempt_slots=ledger['unknown_preempt_slots'],stopped_reason=ledger.get('stopped_reason'),
                 dimensions=dimensions,metrics=metrics,observations=observations,
                 exact_hardware_preempt_completion='unmeasured',exact_context_save_duration='unmeasured',exact_BG_resume_latency='unmeasured')
    write_json(directory/'paired_analysis.json',summary)
    lines=['# Phase 6 paired observations','',
           f"Planned 10 pairs / 20 runs; started {summary['started_runs']}; complete runs {summary['complete_runs']}; complete pairs {summary['complete_pairs']}.",
           f"Active slots reserved {ledger['active_slots_reserved']}; known PREEMPT attempts {ledger['known_preempt_attempts']}; unresolved slots {ledger['unknown_preempt_slots']}.",
           f"Stopped reason: {ledger.get('stopped_reason') or 'none'}.",
           f"Started pairs {summary['started_pairs']}; missing entry/done among started runs {summary['missing_entry_started_runs']}/{summary['missing_done_started_runs']}; incomplete trials {summary['incomplete_trial_started_runs']}; failed/unconfirmed runs {summary['failed_or_unconfirmed_runs']}; control errors {summary['control_error_rows']}; timeout runs {summary['timeout_runs']}.",
           f"Mechanism timing-eligible samples {summary['mechanism_timing_eligible_samples']} (additional subset only; all application intervals remain in the primary table).",'',
           'Δ = control − treatment. Positive means the treatment observation was earlier. All known application intervals are retained, including RM failures, ambiguous/before-RM ordering, late BG and incorrect outputs. Missing intervals remain unknown; configuration mismatches cannot form a matched delta. This is a small fixed batch, with no p99/SLA or adaptive stopping claim.','',
           '| Pair | Order | Delay μs | C entry μs | T entry μs | Δ entry μs | C done μs | T done μs | Δ done μs | T ordering | Complete/correct |',
           '|---|---|---:|---:|---:|---:|---:|---:|---:|---|---|']
    for p in pairs:
        lines.append(f"| {p['pair_id']} | {p['pair_order']} | {p['planned_delay_us']} | {us(p['control_entry_ns'])} | {us(p['treatment_entry_ns'])} | {us(p['delta_entry_ns'])} | {us(p['control_done_ns'])} | {us(p['treatment_done_ns'])} | {us(p['delta_done_ns'])} | {p['treatment_ordering_relative_to_rm']} | {p['complete_correct_pair']} |")
    for metric,m in metrics.items():
        d=m['delta'];lines+=['',f"{metric}: control median {us(m['control']['median_ns'])} μs (n={m['control']['n']}), treatment median {us(m['treatment']['median_ns'])} μs (n={m['treatment']['n']}).",
            f"Paired Δ: n={d['n']}, median {us(d['median_ns'])}, mean {us(d['mean_ns'])}, range [{us(d['min_ns'])}, {us(d['max_ns'])}] μs; improved/worse/equal={m['improved']}/{m['worse']}/{m['equal']}.",
            f"Complete/correct subset: n={m['complete_correct_subset']['n']}, median Δ {us(m['complete_correct_subset']['median_ns'])} μs."]
        for order,d in m['by_order'].items():lines.append(f"{order}: n={d['n']}, median Δ {us(d['median_ns'])} μs, all deltas ns={d['values_ns']}.")
    lines+=['','Independent dimensions:','',json.dumps(dimensions,indent=2),'',
            'Timing decomposition (all returned/observed rows, μs; no-op has no RM syscall interval):','',
            '| Condition | Metric | n | Median | Min | Max |','|---|---|---:|---:|---:|---:|']
    for cond in ('control','treatment'):
        for key in ('submit_ns','ipc_request_ns','receive_to_prepare_ns','prepare_ns','prepare_to_rm_ns','rm_wall_ns','prepare_to_action_end_ns','ipc_roundtrip_ns','trigger_lateness_ns'):
            values=[o[key] for o in observations if o['condition']==cond and o[key] is not None];d=describe(values)
            lines.append(f"| {cond} | {key} | {d['n']} | {us(d['median_ns'])} | {us(d['min_ns'])} | {us(d['max_ns'])} |")
    lines+=['','Preparation includes lock acquisition, current environment/FD validation, full registry/snapshot and exact GET_INFO checks, and metadata bookkeeping. It is not solely registry scanning.',
            '', 'Host-observed entry/done are application observations. CPU/GPU clocks are not directly subtracted; at/after-RM observation remains ambiguous. No ordering model was enabled. Exact BG preempt completion, context-save duration and BG resume latency remain unmeasured. A lower paired latency does not by itself establish hardware preemption causality, sustained suspension, or a scheduler guarantee.',
            '', 'Final bit-exact outputs and same-context short reuse test final correctness/usability, not absence of replay or instruction-level context save. One captured TSG is not proof of complete CUDA context resource coverage.']
    (directory/'paired_summary.md').write_text('\n'.join(lines)+'\n')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',type=Path);a=p.parse_args();analyze(a.directory)
