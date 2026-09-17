#!/usr/bin/env python3
"""Synthetic diagnostic contracts only; no profiler, RM controls or GPU work."""
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest
from unittest.mock import patch
import run_diagnostic as runner
from analyze_diagnostic import (DOMAIN,REQUIRED,analyze,associate_switch,inspect_schema,log_limits,pid,reviewed_schema,same_clock_delta,select_marker)
from run_diagnostic import profile_command,profiler_environment,validate_d0,belongs_to_run

class DiagnosticContracts(unittest.TestCase):
    def test_modes_and_termination(self):
        for condition in ('D0','D1'):
            cmd=profile_command('nsys',Path('/synthetic/build'),Path('/synthetic/run'), 'GPU-synthetic',condition,condition=='D1')
            self.assertEqual(cmd[cmd.index('--trials')+1],'1')
            self.assertEqual(cmd[cmd.index('--mode')+1],'group-bound-none' if condition=='D0' else 'group-preempt-wait')
            self.assertEqual('--test-host-confirmed' in cmd,condition=='D1')
            for option in ('--kill=none','--duration=0','--wait=all','--trace-fork-before-exec=false','--sample=none','--cpuctxsw=none'):
                self.assertIn(option,cmd)
            self.assertNotIn('--diagnostic-progress',cmd)
        with self.assertRaises(ValueError):profile_command('nsys',Path('/b'),Path('/r'),'g','D1',False)
        with self.assertRaises(ValueError):profile_command('nsys',Path('/b'),Path('/r'),'g','D0',True)

    def test_environment_not_a_secret_export(self):
        env=profiler_environment({'PATH':'/synthetic','HOME':'/synthetic','SECRET_TOKEN':'synthetic','LD_PRELOAD':'unrelated'},'GPU-selected')
        self.assertNotIn('SECRET_TOKEN',env);self.assertNotIn('LD_PRELOAD',env)
        self.assertEqual(env['CUDA_VISIBLE_DEVICES'],'GPU-selected')
        with self.assertRaises(ValueError):profiler_environment({'CUDA_MPS_PIPE_DIRECTORY':'/synthetic'},'GPU-selected')
        self.assertTrue(belongs_to_run(['/build/bg_worker','--run-dir','/synthetic/run'],Path('/synthetic/run')))
        self.assertFalse(belongs_to_run(['/other/program','--run-dir','/synthetic/run'],Path('/synthetic/run')))
        self.assertFalse(belongs_to_run(['/build/bg_worker','--run-dir','/another/task'],Path('/synthetic/run')))
        self.assertFalse(belongs_to_run(['/build/bg_worker','--run-dir'],Path('/synthetic/run')))

    def test_context_ids_are_not_tsg_ids(self):
        identities={'BG':{'pid':12,'hardware_tsg_id':6,'scoped_association_valid':True,'switch_association_window_ns':[100,200]},
                    'INT':{'pid':13,'hardware_tsg_id':10,'scoped_association_valid':True,'switch_association_window_ns':[110,200]}}
        r={'globalPid':(1<<48)+(12<<24),'contextId':900,'gpuId':0,'timestamp':150}
        role,method=associate_switch(r,identities,{'BG':{900},'INT':{901}},0)
        self.assertEqual(role,'BG');self.assertIn('not_ID_equivalence',method);self.assertEqual(pid(r['globalPid']),12)
        for changed,expected in [({'contextId':0},'anonymous_or_unknown'),({'globalPid':0},'anonymous_or_unknown'),
                                 ({'timestamp':99},'outside_workload_association_window'),({'gpuId':1},'outside_this_run_scope')]:
            self.assertEqual(associate_switch(dict(r,**changed),identities,{'BG':{900}},0)[1],expected)
        self.assertEqual(associate_switch(r,identities,{'BG':{900,901}},0)[1],'multiple_switch_contexts_in_trial')
        identities['BG']['scoped_association_valid']=False
        self.assertEqual(associate_switch(r,identities,{'BG':{900}},0)[1],'context_or_binding_ambiguous')

    def test_parent_only_and_failed_d0_never_authorize(self):
        parent=[{'label':'AP/phase7/synthetic/INT/13/0/int_submit','globalTid':13<<24}]
        self.assertEqual(select_marker(parent,'INT','int_submit',13),parent[0])
        with self.assertRaises(ValueError):select_marker(parent,'BG','bg_submit',12)
        with self.assertRaises(ValueError):select_marker(parent,'INT','int_submit',14)
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'analysis').mkdir()
            for cause in ('parent-only trace missing BG','zero_events','unknown_export_schema','truncated','active failure'):
                (p/'analysis/assessment.json').write_text(json.dumps({'backend_ready_for_d1':False,'limitations':[cause]}))
                with self.assertRaises(ValueError):validate_d0(p,Path('/no-build-access'),'GPU-synthetic')

    def test_unknown_schema_rejects(self):
        c=sqlite3.connect(':memory:');c.row_factory=sqlite3.Row
        c.execute('create table SOMETHING_NEW(timestamp integer)')
        self.assertFalse(reviewed_schema(c,inspect_schema(c))[0])
        for t,columns in REQUIRED.items():c.execute('create table '+t+' ('+','.join(k+' TEXT' for k in columns)+')')
        c.executemany('insert into META_DATA_EXPORT values(?,?)',[('EXPORT_PRODUCT_VERSION','unreviewed'),('EXPORT_SCHEMA_VERSION','3.16.1')])
        # Column order comes from a set: rebuild metadata deterministically.
        c.execute('drop table META_DATA_EXPORT');c.execute('create table META_DATA_EXPORT(name text,value text)')
        c.executemany('insert into META_DATA_EXPORT values(?,?)',[('EXPORT_PRODUCT_VERSION','2024.6.2.225'),('EXPORT_SCHEMA_VERSION','3.16.1')])
        self.assertTrue(reviewed_schema(c,inspect_schema(c))[0])
        c.execute("update META_DATA_EXPORT set value='future' where name='EXPORT_PRODUCT_VERSION'")
        self.assertFalse(reviewed_schema(c,inspect_schema(c))[0])

    def test_clocks_and_raw_operations(self):
        large=10**18
        self.assertEqual(same_clock_delta(large+17,DOMAIN,large,DOMAIN),17)
        with self.assertRaises(ValueError):same_clock_delta(large,DOMAIN,large,'CLOCK_MONOTONIC_RAW')
        errors,loss=log_limits([{'text':'truncated capture','severity':2}],{2:'Warning'})
        self.assertFalse(errors);self.assertEqual(len(loss),1)
        errors,loss=log_limits([{'text':'record buffer overflow','severity':3}],{3:'Error'})
        self.assertEqual(len(errors),1);self.assertEqual(len(loss),1)

    def test_missing_export_preserves_stop_reason(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'collection.log').write_text('synthetic permission denied')
            r=analyze(p,p/'analysis')
            self.assertFalse(r['backend_ready_for_d1']);self.assertEqual(r['status'],'permission_denied')
            self.assertIsNone(r['dropped_records']);self.assertTrue((p/'analysis/assessment.json').is_file())
            self.assertFalse((p/'raw.csv').exists())

    def test_interrupted_active_keeps_reservation_and_unknown(self):
        # All transports/readiness are mocks, not a GPU run or an authorization.
        with tempfile.TemporaryDirectory(prefix='synthetic-diagnostic-') as tmp:
            p=Path(tmp);(p/'D0').mkdir()
            args=['run_diagnostic','--build',str(p/'build'),'--output',str(p/'D1'),
                  '--gpu','GPU-00000000-0000-0000-0000-000000000001','--condition','D1','--d0',str(p/'D0'),'--test-host-confirmed']
            with patch('sys.argv',args),patch.dict('os.environ',{},clear=True),patch.object(runner.shutil,'which',return_value='synthetic-nsys'), \
                 patch.object(runner,'validate_d0'),patch.object(runner,'binary_fingerprints',return_value={}), \
                 patch.object(runner,'command',return_value={'stdout':'synthetic','returncode':0}), \
                 patch.object(runner,'collect',return_value={}),patch.object(runner,'readiness_check'), \
                 patch.object(runner,'run_process_group',side_effect=OSError('synthetic collection interruption')) as transport,patch('builtins.print'):
                self.assertEqual(runner.main(),1);self.assertEqual(transport.call_count,1)
            result=json.loads((p/'D1/result.json').read_text())
            self.assertIsNone(result['active_attempts']);self.assertTrue(result['active_attempts_unknown'])
            self.assertEqual(result['state'],'STOPPED');self.assertTrue((p/'D0/d1_reserved.json').exists())
            self.assertFalse((p/'D1/worker/raw.csv').exists())

if __name__=='__main__':unittest.main()
