#!/usr/bin/env python3
"""Synthetic fixtures in temporary directories only. These are not GPU results."""
import csv
import json
from pathlib import Path
import tempfile
import unittest
from summarize import quantiles,summarize,ordering,CalibrationModel,read_rows

def row(**updates):
    fields='schema_version trial mode run_kind force bypass graph bg_present trial_state T_cpu_trigger T_int_submit_begin T_int_submit_end T_ipc_send T_ipc_received T_ipc_ack T_rm_call_begin T_rm_call_end rm_syscall_result rm_errno rm_status operation_seq T_int_graph_entry_observed T_int_main_entry_observed T_int_graph_done_observed T_bg_main_observed T_int_graph_entry_gpu_ns T_int_main_entry_gpu_ns T_int_graph_done_gpu_ns T_bg_main_gpu_ns T_bg_done_gpu_ns int_entry_node int_main_node launch_id bg_done_before_interaction bg_done_before_control_observed control_target_running application_valid control_status gpu_overlap ordering_relative_to_rm correctness_status bg_correct int_correct diagnostic_progress_correct observer_max_poll_gap_ns heartbeat_overflow T_bg_preempted_observed T_bg_resumed_observed failure'.split()
    r=dict.fromkeys(fields,'')
    r.update(schema_version='2',trial='0',mode='preempt-wait',run_kind='performance',graph='0',bg_present='1',trial_state='complete',
             T_cpu_trigger='1000',T_int_submit_begin='1100',T_int_submit_end='2000',T_rm_call_begin='3000',T_rm_call_end='5000',
             rm_syscall_result='0',rm_status='0',T_int_graph_entry_observed='7000',T_int_main_entry_observed='7000',T_int_graph_done_observed='9000',
             T_int_graph_entry_gpu_ns='100000',T_int_main_entry_gpu_ns='100000',T_int_graph_done_gpu_ns='400000',bg_done_before_interaction='0',bg_done_before_control_observed='0',
             control_target_running='unknown',application_valid='1',control_status='CONTROL_ACCEPTED_EFFECT_UNVERIFIED',gpu_overlap='lifetime_overlap',
             ordering_relative_to_rm='ordering_ambiguous',correctness_status='PASS',bg_correct='1',int_correct='1',observer_max_poll_gap_ns='100')
    r.update(updates);return r

def write_rows(path,rows):
    with path.open('w') as f:w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)

class AnalysisTest(unittest.TestCase):
    def test_percentiles(self):
        self.assertIsNone(quantiles([]));self.assertEqual(quantiles([7]),[7,7,7,7]);self.assertEqual(quantiles([0,100])[0],50)
    def test_delayed_observer_is_ambiguous(self):
        self.assertEqual(ordering(row()),'ordering_ambiguous')
        self.assertEqual(ordering(row(T_int_graph_entry_observed='2500')),'before_rm')
        self.assertEqual(ordering(row(T_rm_call_begin='')),'not_applicable')
    def test_graph_entry_before_rm_main_after(self):
        self.assertEqual(ordering(row(graph='1',T_int_graph_entry_observed='2500',T_int_main_entry_observed='7000')),'before_rm')
    def test_all_application_samples_and_independent_failures(self):
        rows=[row(),row(control_status='IOCTL_FAILURE',rm_syscall_result='-1',rm_status='0'),
              row(T_int_graph_entry_observed='2500'),row(trial_state='incomplete',application_valid='0',correctness_status='unknown',failure='INT timeout'),
              row(correctness_status='CORRECTNESS_FAILURE',int_correct='0')]
        with tempfile.TemporaryDirectory(prefix='ap-synthetic-analysis-') as d:
            p=Path(d);write_rows(p/'raw.csv',rows);text=summarize(p).read_text();stats=json.loads((p/'analysis.json').read_text())
            self.assertEqual(stats['application_valid'],4);self.assertEqual(stats['timing_eligible_subset'],0);self.assertEqual(stats['preemption_confirmed'],0)
            self.assertEqual(stats['dimensions']['derived_ordering']['before_rm'],1)
            self.assertEqual(stats['dimensions']['control_status']['IOCTL_FAILURE'],1);self.assertIn('**unmeasured**',text)
    def test_empty_truncated_legacy_mixed_rejected(self):
        with tempfile.TemporaryDirectory(prefix='ap-synthetic-analysis-') as d:
            p=Path(d)/'raw.csv';p.write_text('one,two\n')
            with self.assertRaises(ValueError):read_rows(p)
            p.write_text('one,two\n1\n')
            with self.assertRaises(ValueError):read_rows(p)
            for rows in ([row(schema_version='1')],[row(),row(schema_version='1')],[row(),row(run_kind='diagnostic')]):
                write_rows(p,rows)
                with self.assertRaises(ValueError):read_rows(p)
    def test_explicit_model_and_no_extrapolation(self):
        with tempfile.TemporaryDirectory(prefix='ap-synthetic-clock-') as d:
            p=Path(d)
            (p/'before.csv').write_text('cpu_send_ns,gpu_ns,cpu_observed_ns\n100,1000,120\n')
            (p/'after.csv').write_text('cpu_send_ns,gpu_ns,cpu_observed_ns\n1100,2000,1120\n')
            model=CalibrationModel(p/'before.csv',p/'after.csv',10)
            r=row(T_int_graph_entry_gpu_ns='1500',T_rm_call_begin='500',T_int_graph_entry_observed='1000')
            self.assertEqual(ordering(r),'ordering_ambiguous');self.assertEqual(ordering(r,model),'after_rm_under_calibration_model')
            self.assertEqual(ordering(dict(r,T_int_graph_entry_gpu_ns='3000'),model),'ordering_ambiguous')
            self.assertIsNone(model.interval(999))
    def test_int_only_has_no_fabricated_bg(self):
        r=row(mode='int-only',bg_present='0',bg_correct='',gpu_overlap='not_applicable',T_bg_main_gpu_ns='',T_bg_done_gpu_ns='',T_rm_call_begin='',T_rm_call_end='',control_status='NOT_ISSUED')
        self.assertEqual(ordering(r),'not_applicable')

if __name__=='__main__':unittest.main()
