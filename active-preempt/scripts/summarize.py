#!/usr/bin/env python3
"""Summarize measured rows only; exact hardware preempt/resume remains unobserved."""
import argparse
from collections import Counter
import csv
from pathlib import Path

def quantiles(values):
    if not values:
        return None
    values = sorted(values)
    def q(p):
        i = (len(values) - 1) * p
        a = int(i)
        return values[a] + (values[min(a + 1, len(values)-1)] - values[a]) * (i-a)
    return [q(.50), q(.95), q(.99), values[-1]]

def delta(row, end, begin):
    if not row.get(end) or not row.get(begin):
        return None
    x = int(row[end]) - int(row[begin])
    return x / 1000 if x >= 0 else None

def summarize(directory):
    directory = Path(directory)
    with (directory / "raw.csv").open() as f:
        reader = csv.DictReader(f)
        rows = list(reader)
    if not rows:
        raise ValueError("No measured rows; refusing synthetic summary")
    if any(None in r or any(v is None for v in r.values()) for r in rows):
        raise ValueError("Malformed CSV field count")
    good = [r for r in rows if r["bg_correct"] == r["int_correct"] == "1"
            and r["classification"] in {"observed", "int_before_rm_issue", "int_after_bg_done"}
            and (not r["T_rm_call_begin"] or (r["rm_syscall_result"] == "0" and r["rm_status"] == "0"))]
    trigger_subset = [r for r in good if r["classification"] == "observed"]
    lines = ["# Measured microbenchmark summary", "", f"Rows: {len(rows)}; valid application/observation rows: {len(good)}; rejected: {len(rows)-len(good)}.",
             f"Classifications: {dict(Counter(r['classification'] for r in rows))}.", "",
             "GPU overlap alone does not establish that a particular RM request caused preemption. The no-explicit-preemption control and timeline are still required.", "",
             "| Measurement (μs) | n | p50 | p95 | p99 | max |", "|---|---:|---:|---:|---:|---:|"]
    metrics = [
        ("Interaction → INT start observed (all valid)", good, "T_int_gpu_start_observed", "T_cpu_trigger"),
        ("Interaction → INT start (observed classification only)", trigger_subset, "T_int_gpu_start_observed", "T_cpu_trigger"),
        ("RM syscall wall time, including transport/waits", good, "T_rm_call_end", "T_rm_call_begin"),
        ("Host launch API wall time", good, "T_int_submit_end", "T_cpu_trigger"),
        ("INT GPU marker interval", good, "T_int_gpu_done_ns", "T_int_gpu_start_ns"),
        ("Re-enable syscall wall time", good, "T_reenable_end", "T_reenable_begin"),
        ("BG sentinel gap enclosing INT start (proxy only)", good, "T_bg_gap_end_gpu_proxy", "T_bg_gap_begin_gpu_proxy"),
    ]
    for name, source, end, begin in metrics:
        values = [x for r in source if (x := delta(r, end, begin)) is not None]
        q = quantiles(values)
        lines.append(f"| {name} | {len(values)} | " + (" | ".join(f"{v:.3f}" for v in q) if q else "N/A | N/A | N/A | N/A") + " |")
    lines += ["", "Exact preemption completion latency, context-switch duration, and BG resume latency: **unmeasured**. Synchronous control return is at most an interface-contract completion bound, including CPU/RPC overhead. Empty exact-event fields are intentional.",
              "", "The heartbeat gap is from one CTA; it neither proves full-TSG inactivity nor distinguishes scheduler rotation from the explicit request. No constant overhead is subtracted.", ""]
    for f in sorted(directory.glob("*_calibration_*.csv")):
        with f.open() as stream:
            samples = list(csv.DictReader(stream))
        if not samples:
            continue
        rtts = [(int(s["cpu_observed_ns"]) - int(s["cpu_send_ns"])) / 1000 for s in samples]
        best = min(samples, key=lambda s: int(s["rtt_ns"]))
        low = int(best["cpu_send_ns"]) - int(best["gpu_ns"])
        high = int(best["cpu_observed_ns"]) - int(best["gpu_ns"])
        lines += [f"{f.name}: mapped ping RTT p50/p95/p99/max μs = {quantiles(rtts)}; best CPU−GPU offset bracket ns = [{low}, {high}]. This brackets transport/observation; it is not a one-way latency measurement.", ""]
    polling = [int(r["observer_max_poll_gap_ns"]) / 1000 for r in good]
    lines += [f"Observer maximum polling gaps per trial, p50/p95/p99/max μs: {quantiles(polling)}.", "",
              "Check calibration drift (before vs after), heartbeat perturbation in identity files, CTA duration distribution, and any overflow before interpreting microsecond differences."]
    output = directory / "summary.md"
    output.write_text("\n".join(lines) + "\n")
    return output

if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("directory", type=Path)
    a = p.parse_args()
    print(summarize(a.directory))
