#!/usr/bin/env python3
"""Offline synthetic contracts. No GPU or RM syscall; files only in temp dirs."""
import copy
import csv
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from pair_contract import *
from analyze_pairs import difference_ns,paired_differences,describe,analyze
from run_paired import control_facts,validate_readiness
from summarize import ordering


def fixture_plan():
    c=dict(schema_version=2,measurement_contract='group-preparation-v1',graph=0,force=0,bypass=0,run_kind='performance',
           threads_per_block=256,dynamic_shared_bytes=65536,heartbeat_ns=2000,bg_iterations=1630976,int_iterations=5888,cta_waves=1,
           gpu_uuid='00112233445566778899aabbccddeeff',trigger_delay_us=3000,cpu_affinity=[0,1],bg_cpu_affinity=[0,1],
           build_profile='synthetic',nvidia_source_commit='synthetic-only',initialization_policy='fixture',sm_count=108)
    p=dict(batch_id='synthetic-only',seed=20260917,frozen_configuration=c,configuration_sha256=digest(c),
           max_preempt_attempts=10,binary_sha256={'synthetic':'fixture'},pairs=make_schedule('synthetic-only'))
    p['plan_sha256']=digest(p);return p


class Schedule(unittest.TestCase):
    def test_balanced_fixed_plan(self):
        p=fixture_plan();check_plan(p);self.assertEqual(p['pairs'],make_schedule(p['batch_id']))
        self.assertEqual([a['pair_order'] for a in p['pairs']].count('CT'),5)
        self.assertEqual([a['pair_order'] for a in p['pairs']].count('TC'),5)
        ids=[]
        for pair in p['pairs']:
            self.assertTrue(1000<=pair['trigger_delay_us']<=5000)
            self.assertEqual({r['condition'] for r in pair['runs']},{'control','treatment'})
            self.assertEqual([r['local_trial_id'] for r in pair['runs']],[0,0])
            ids += [r['run_id'] for r in pair['runs']]
        self.assertEqual(len(ids),len(set(ids)))
    def test_mutation_and_budget_rejected(self):
        for key,value in [('max_preempt_attempts',11),('seed',7)]:
            p=fixture_plan();p[key]=value
            with self.assertRaises(ValueError):check_plan(p)
    def test_no_unconfirmed_treatment(self):
        p=fixture_plan();pair=p['pairs'][0]
        for r in pair['runs']:
            if r['condition']=='treatment':
                with self.assertRaises(ValueError):command_for('/build','/out',p,pair,r,False)
            cmd=command_for('/build','/out',p,pair,r,True)
            self.assertEqual('--test-host-confirmed' in cmd,r['condition']=='treatment')
            self.assertEqual(cmd[cmd.index('--trials')+1],'1')
            self.assertEqual(cmd[cmd.index('--pair-id')+1],str(pair['pair_id']))
            self.assertNotIn('--local-trial-id',cmd)
    def test_unknown_failure_no_retry(self):
        p=fixture_plan();l=empty_ledger(p);pair=p['pairs'][0];r=pair['runs'][0]
        reserve(l,p,pair,r)
        with self.assertRaises(ValueError):reserve(l,p,pair,r)
        with self.assertRaises(ValueError):reserve(l,p,pair,pair['runs'][1])
        l['stopped_reason']='timeout'
        with self.assertRaises(ValueError):reserve(l,p,pair,r)
    def test_all_slots_consumed_including_failed_attempts(self):
        p=fixture_plan();l=empty_ledger(p)
        for pair in p['pairs']:
            for run in pair['runs']:
                record=reserve(l,p,pair,run)
                record['preempt_attempts']=int(run['condition']=='treatment');record['state']='COMPLETE';recount(l)
        self.assertEqual((l['active_slots_reserved'],l['known_preempt_attempts']),(10,10))
        with self.assertRaises(ValueError):reserve(l,p,p['pairs'][0],p['pairs'][0]['runs'][0])
    def test_reservation_unknown_conservative(self):
        p=fixture_plan();l=empty_ledger(p)
        for pair in p['pairs']:
            for run in pair['runs']:
                r=reserve(l,p,pair,run)
                if run['condition']=='treatment':
                    self.assertEqual(l['unknown_preempt_slots'],1)
                    self.assertEqual(l['active_slots_reserved'],1)
                    return
                r['state']='COMPLETE'
        self.fail('no treatment')
    def test_serialized_reservation_before_launch(self):
        p=fixture_plan();l=empty_ledger(p);pair=p['pairs'][0]
        with tempfile.TemporaryDirectory(prefix='ap-synthetic-ledger-') as directory:
            reserve(l,p,pair,pair['runs'][0]);write_json(Path(directory)/'attempts.json',l)
            loaded=json.loads((Path(directory)/'attempts.json').read_text())
            self.assertEqual(loaded['runs'][0]['state'],'RESERVED_OUTCOME_UNKNOWN')
            with self.assertRaises(ValueError):reserve(loaded,p,pair,pair['runs'][0])
    def test_shared_config_exclusions_are_narrow(self):
        p=fixture_plan();pair=p['pairs'][0]
        a=dict(expected_config(p,pair),mode='group-bound-none',batch_id='x',run_id='1',pair_id='1',condition='control',pair_order='CT')
        b=dict(a,mode='group-preempt-wait',run_id='2',condition='treatment')
        self.assertEqual(config_fingerprint(a),config_fingerprint(b))
        for key in ('bg_iterations','int_iterations','heartbeat_ns','dynamic_shared_bytes','trigger_delay_us','cpu_affinity','gpu_uuid','build_profile','initialization_policy','new_unknown_config'):
            c=dict(b);c[key]='changed';self.assertNotEqual(config_fingerprint(a),config_fingerprint(c),key)
    def test_plan_workload_and_run_metadata(self):
        p=fixture_plan();pair=p['pairs'][0];run=pair['runs'][0]
        c=dict(expected_config(p,pair),batch_id=p['batch_id'],pair_id=str(pair['pair_id']),pair_order=pair['pair_order'],**{k:run[k] for k in ('condition','mode','run_id')})
        validate_configuration(c,p,pair,run)
        c['mode']='none'
        with self.assertRaises(ValueError):validate_configuration(c,p,pair,run)
    def test_cuda_missing_explicit_stop(self):
        with self.assertRaisesRegex(ValueError,'DEVICE_ACCESS_BLOCKED'):validate_readiness({'cuda_usable':False},fixture_plan())


class Analysis(unittest.TestCase):
    def test_integer_first_subtraction(self):
        base=2**60
        self.assertEqual(difference_ns({'start':str(base),'end':str(base+19)},'end','start'),19)
        self.assertIsNone(difference_ns({'start':str(base)},'end','start'))
    def test_difference_sign_and_medians(self):
        pairs=[dict(configuration_match=True,control_entry_ns=c,treatment_entry_ns=t) for c,t in [(10,20),(20,5),(30,30),(100,20)]]
        delta=paired_differences(pairs,'entry');self.assertEqual(delta,[-10,15,0,80]);self.assertEqual(describe(delta)['median_ns'],7.5)
        self.assertEqual(describe([1,2,100])['median_ns'],2)
        self.assertEqual(describe([1,2,3,100])['median_ns'],2.5)
        pairs[0]['configuration_match']=False;self.assertEqual(paired_differences(pairs,'entry'),[15,0,80])
    def test_delayed_observer_ambiguous(self):
        row={'T_rm_call_begin':'10000','T_int_graph_entry_observed':'11000','T_int_graph_entry_gpu_ns':'1'}
        self.assertEqual(ordering(row),'ordering_ambiguous')
        row['T_int_graph_entry_observed']='9999';self.assertEqual(ordering(row),'before_rm')
    def test_no_gpu_no_fake_pairs(self):
        with tempfile.TemporaryDirectory(prefix='ap-synthetic-plan-') as directory:
            p=fixture_plan();d=Path(directory);write_json(d/'plan.json',p);write_json(d/'attempts.json',empty_ledger(p));analyze(d)
            self.assertFalse((d/'pairs.csv').exists());self.assertFalse((d/'paired_analysis.json').exists())
    def test_journal_separates_noop_get_and_real_requests(self):
        with tempfile.TemporaryDirectory(prefix='ap-synthetic-journal-') as directory:
            d=Path(directory)
            for role in ('bg','int'):
                events=[dict(event_kind='RM_CONTROL',command=0xa06c0106,attempted=True,operation_state='RETURNED')]
                if role=='bg':events+=[dict(event_kind='CONTROL_PREPARATION_NOOP',command=None,attempted=False,operation_state='LOCAL_PREPARATION_COMPLETED')]
                (d/f'{role}_control_events.jsonl').write_text(''.join(json.dumps(e)+'\n' for e in events))
            f=control_facts(d);self.assertEqual((f['get_info'],f['preempt'],f['pending']),(2,0,0))
            with (d/'bg_control_events.jsonl').open('a') as out:out.write(json.dumps(dict(event_kind='RM_CONTROL',command=0xa06c0105,attempted=None,operation_state='IN_FLIGHT'))+'\n')
            self.assertTrue(control_facts(d)['preempt_unknown'])
    def test_adverse_rows_retained(self):
        with tempfile.TemporaryDirectory(prefix='ap-synthetic-analysis-') as directory:
            d=Path(directory);p=fixture_plan();l=empty_ledger(p);pair=p['pairs'][0]
            rows={}
            for run in pair['runs']:
                rec=reserve(l,p,pair,run);rec['state']='COMPLETE';rec['preempt_attempts']=int(run['condition']=='treatment')
                out=d/run['run_id'];out.mkdir();(out/'raw.csv').touch()
                write_json(out/'configuration.json',expected_config(p,pair))
                offset=500 if run['condition']=='control' else 1000
                rows[out/'raw.csv']=[dict(T_cpu_trigger=str(2**60),T_int_graph_entry_observed=str(2**60+offset),T_int_graph_done_observed=str(2**60+offset+100),
                     application_valid='1',correctness_status='CORRECTNESS_FAILURE',trial_state='incomplete',control_status='CONTROL_TIMEOUT',T_rm_call_begin=str(2**60+750),
                     gpu_overlap='unknown',bg_done_before_interaction='1')]
            recount(l);write_json(d/'plan.json',p);write_json(d/'attempts.json',l)
            with patch('analyze_pairs.read_rows',side_effect=lambda path:rows[path]):analyze(d)
            result=json.loads((d/'paired_analysis.json').read_text())
            self.assertEqual(result['metrics']['entry']['delta']['values_ns'],[-500])
            self.assertEqual(result['metrics']['entry']['worse'],1)
            self.assertEqual(result['metrics']['entry']['complete_correct_subset']['n'],0)
            self.assertEqual(result['dimensions']['treatment']['control_status']['CONTROL_TIMEOUT'],1)
            self.assertIn('unknown',(d/'paired_summary.md').read_text())

if __name__=='__main__':unittest.main()
