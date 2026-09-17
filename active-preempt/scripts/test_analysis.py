#!/usr/bin/env python3
"""Offline analysis tests. Temporary fixtures are NOT GPU benchmark measurements."""
import csv
from pathlib import Path
import tempfile
import unittest
from summarize import quantiles, summarize

class AnalysisTest(unittest.TestCase):
    def test_percentiles(self):
        self.assertEqual(quantiles([]), None)
        self.assertEqual(quantiles([7]), [7, 7, 7, 7])
        self.assertEqual(quantiles([0, 100])[0], 50)
        self.assertEqual(quantiles([0, 100])[2], 99)

    def test_failed_rm_and_preexisting_int_are_not_causal_success(self):
        fields = "bg_correct int_correct classification T_cpu_trigger T_int_submit_end T_rm_call_begin T_rm_call_end rm_syscall_result rm_status T_int_gpu_start_observed T_int_gpu_done_ns T_int_gpu_start_ns T_reenable_end T_reenable_begin T_bg_gap_end_gpu_proxy T_bg_gap_begin_gpu_proxy observer_max_poll_gap_ns".split()
        row = dict.fromkeys(fields, "")
        row.update(bg_correct="1", int_correct="1", classification="observed", T_cpu_trigger="1000", T_int_submit_end="2000",
                   T_rm_call_begin="3000", T_rm_call_end="5000", rm_syscall_result="0", rm_status="0",
                   T_int_gpu_start_observed="7000", T_int_gpu_start_ns="100000", T_int_gpu_done_ns="400000", observer_max_poll_gap_ns="100")
        bad = dict(row, classification="rm_error", rm_syscall_result="-1", rm_status="0")
        early = dict(row, classification="int_before_rm_issue", T_int_gpu_start_observed="2500")
        with tempfile.TemporaryDirectory(prefix="ap-unit-fixture-") as d:
            with (Path(d)/"raw.csv").open("w") as f:
                writer=csv.DictWriter(f, fieldnames=fields);writer.writeheader();writer.writerows([row,bad,early])
            text=summarize(d).read_text()
            self.assertIn("valid application/observation rows: 2; rejected: 1",text)
            self.assertIn("observed classification only) | 1 |",text)
            self.assertIn("**unmeasured**",text)

    def test_empty_and_truncated_rows_rejected(self):
        with tempfile.TemporaryDirectory(prefix="ap-unit-fixture-") as d:
            p=Path(d)/"raw.csv"
            p.write_text("one,two\n")
            with self.assertRaises(ValueError): summarize(d)
            p.write_text("one,two\n1\n")
            with self.assertRaises(ValueError): summarize(d)

if __name__=="__main__":
    unittest.main()
