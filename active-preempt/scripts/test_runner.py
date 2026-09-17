#!/usr/bin/env python3
"""Offline admission/probe/process-tree tests; no CUDA workload or RM control."""
import argparse
import json
import os
from pathlib import Path
import signal
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
from run_matrix import canonical,run_process_group,validate_admission
from preflight import command,collect
from test_analysis import row,write_rows

def options(**extra):
    r=dict(modes=['none'],trials=1,test_host_confirmed=False,allow_extended=False,force=0,bypass=0,
           diagnostic_progress=False,graph=False,graph_evidence_reviewed=False,primitive_evidence=None,smoke_evidence=None,
           bg_iterations=0,int_iterations=0,cta_waves=1,heartbeat_ns=2000,timeslice_us=1)
    r.update(extra);return argparse.Namespace(**r)

class RunnerTest(unittest.TestCase):
    def test_modes_and_admission(self):
        self.assertEqual(canonical('realtime'),'realtime-restart')
        for mode in ('none','int-only'):validate_admission(options(modes=[mode]))
        for mode in ('timeslice','preempt-wait','realtime-only','realtime','disable'):
            with self.assertRaises(ValueError):validate_admission(options(modes=[mode]))
        validate_admission(options(modes=['preempt-wait'],test_host_confirmed=True))
        with self.assertRaises(ValueError):validate_admission(options(modes=['preempt-async'],test_host_confirmed=True))
        with self.assertRaises(ValueError):validate_admission(options(graph=True))
    def test_no_automatic_large_samples(self):
        with self.assertRaises(ValueError):validate_admission(options(trials=1000))
        with self.assertRaises(ValueError):validate_admission(options(trials=10))
    def test_preflight_missing_tool_has_call_and_errno(self):
        r=command(['/definitely-not-an-ap-tool'])
        self.assertIsNone(r['returncode']);self.assertEqual(r['errno'],2);self.assertIn('call',r)
    def test_missing_device_keeps_stop_reason(self):
        with (patch('preflight.command',side_effect=lambda argv,*a,**kw:dict(call=argv,returncode=77,stdout='',stderr='synthetic CUDA_ERROR_NO_DEVICE')),
             patch('preflight.os.open',side_effect=FileNotFoundError(2,'synthetic missing device')),
             patch('preflight.ctypes.CDLL',side_effect=OSError('synthetic unavailable'))):
            r=collect(Path('/synthetic/no-cuda'))
        self.assertEqual(r['readiness'],'DEVICE_ACCESS_BLOCKED');self.assertFalse(r['cuda_usable'])
        self.assertIn('CUDA_ERROR_NO_DEVICE',r['cuda_probe']['stderr']);self.assertEqual(r['scheduling_controls_issued'],0)
    def test_matching_smoke_freezes_work_and_rejects_configuration_changes(self):
        with tempfile.TemporaryDirectory(prefix='ap-synthetic-admission-') as d:
            p=Path(d);(p/'status.txt').write_text('COMPLETED: synthetic fixture, not GPU evidence\n')
            for role in ('int','bg'):(p/f'{role}_recovery.txt').write_text('CONFIGURATION_RESTORED\n')
            write_rows(p/'raw.csv',[row()])
            config=dict(schema_version=2,mode='preempt-wait',graph=0,run_kind='performance',force=0,bypass=0,
                        cta_waves=1,heartbeat_ns=2000,timeslice_us=1,bg_iterations=8192,int_iterations=256)
            (p/'configuration.json').write_text(json.dumps(config))
            common=dict(modes=['preempt-wait'],trials=10,test_host_confirmed=True,smoke_evidence=p)
            a=options(**common);validate_admission(a)
            self.assertEqual((a.bg_iterations,a.int_iterations),(8192,256))
            for changed in (dict(bg_iterations=4096),dict(cta_waves=2),dict(heartbeat_ns=0),dict(timeslice_us=2),dict(force=1,allow_extended=True)):
                with self.assertRaises(ValueError):validate_admission(options(**common,**changed))
            (p/'bg_recovery.txt').write_text('RECOVERY_FAILED\n')
            with self.assertRaises(ValueError):validate_admission(options(**common))
    def test_spawn_failure_keeps_errno(self):
        with tempfile.TemporaryDirectory(prefix='ap-synthetic-spawn-') as d:
            with (Path(d)/'log').open('w') as f:r=run_process_group(['/definitely-not-an-ap-tool'],os.environ.copy(),f,1)
            self.assertEqual(r['returncode'],127);self.assertEqual(r['spawn_errno'],2);self.assertEqual(r['recovery'],'not_started')
    def test_owner_and_descendant_timeout_cleanup(self):
        with tempfile.TemporaryDirectory(prefix='ap-synthetic-process-tree-') as d:
            p=Path(d);pidfile=p/'descendant.pid'
            script=p/'parent.py'
            script.write_text('import os, signal, subprocess, sys, time\n'
                'signal.signal(signal.SIGTERM, signal.SIG_IGN)\n'
                'child=subprocess.Popen([sys.executable,"-c","import signal,time; signal.signal(signal.SIGTERM,signal.SIG_IGN); time.sleep(20)"])\n'
                'open(sys.argv[1],"w").write(str(child.pid))\n'
                'time.sleep(20)\n')
            with (p/'process.log').open('w') as log:r=run_process_group([sys.executable,str(script),str(pidfile)],os.environ.copy(),log,.4,grace=.15)
            self.assertTrue(r['timed_out']);self.assertEqual(r['returncode'],124)
            self.assertIn('SIGKILL_PROCESS_GROUP_RECOVERY_UNCONFIRMED',r['termination'])
            pid=int(pidfile.read_text());stat=Path(f'/proc/{pid}/stat')
            deadline=time.monotonic()+2
            while stat.exists() and stat.read_text().split()[2]!='Z' and time.monotonic()<deadline:time.sleep(.01)
            self.assertTrue(not stat.exists() or stat.read_text().split()[2]=='Z','live orphan remained')
    def test_normal_process_has_no_forced_cleanup(self):
        with tempfile.TemporaryDirectory(prefix='ap-synthetic-process-') as d:
            with (Path(d)/'log').open('w') as f:r=run_process_group([sys.executable,'-c','pass'],os.environ.copy(),f,2,grace=.1)
        self.assertEqual(r['returncode'],0);self.assertEqual(r['termination'],[])

if __name__=='__main__':unittest.main()
