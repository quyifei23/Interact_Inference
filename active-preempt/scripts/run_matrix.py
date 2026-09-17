#!/usr/bin/env python3
"""Run the finite two-process experiment; never install modules or grant capabilities."""
import argparse
import datetime
import json
import os
from pathlib import Path
import subprocess
import sys

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=Path(__file__).resolve().parents[1] / "build")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--trials", type=int, default=1000)
    parser.add_argument("--modes", nargs="+", default=["none", "timeslice", "preempt-wait", "preempt-async", "realtime", "disable"])
    parser.add_argument("--cta-waves", type=int, default=1)
    parser.add_argument("--heartbeat-ns", type=int, default=2000)
    parser.add_argument("--graph", action="store_true", help="Only after inspecting a successful plain-kernel primitive run")
    parser.add_argument("--primitive-evidence", type=Path, help="Required for graph: successful plain-kernel run directory")
    args = parser.parse_args()
    if args.trials < 1 or args.trials > 100000:
        parser.error("trials must be in 1..100000")
    allowed = {"none", "timeslice", "preempt-wait", "preempt-async", "realtime", "disable", "disable-split"}
    if not set(args.modes) <= allowed:
        parser.error("unknown mode")
    if args.graph:
        import csv
        evidence = args.primitive_evidence
        if not evidence or not (evidence / "status.txt").is_file():
            parser.error("graph phase needs a completed plain-kernel run (--primitive-evidence)")
        with (evidence / "raw.csv").open() as f:
            rows = list(csv.DictReader(f))
        passed = [r for r in rows if r["mode"] in {"preempt-wait", "preempt-async", "realtime", "disable", "disable-split"}
                  and r["graph"] == "0" and r["classification"] == "observed"
                  and r["bg_correct"] == r["int_correct"] == "1"
                  and r["rm_syscall_result"] == r["rm_status"] == "0"]
        if not passed:
            parser.error("evidence has no successful active-primitive overlap with preserved output")
        proven_modes = {r["mode"] for r in passed}
        if not (set(args.modes) - {"none", "timeslice"}) <= proven_modes:
            parser.error("graph active modes must match the supplied primitive evidence")
        print("Graph admission checks overlap and correctness; it does not independently prove causal hardware preemption.")
    args.output.mkdir(parents=True, exist_ok=False)
    binary = (args.build / "int_worker").resolve()
    env = os.environ.copy()
    lib = str((args.build / "librm_control.so").resolve())
    env["LD_PRELOAD"] = lib + (":" + env["LD_PRELOAD"] if env.get("LD_PRELOAD") else "")
    metadata = {"utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "binary": str(binary),
                "trials": args.trials, "cta_waves": args.cta_waves, "graph": args.graph,
                "heartbeat_ns": args.heartbeat_ns, "primitive_evidence": str(args.primitive_evidence) if args.primitive_evidence else None}
    for key, cmd in [("uname", ["uname", "-a"]), ("nvidia_smi", ["nvidia-smi", "-q"]),
                     ("driver", ["cat", "/proc/driver/nvidia/version"])]:
        try:
            p = subprocess.run(cmd, text=True, capture_output=True, timeout=20)
            metadata[key] = {"returncode": p.returncode, "stdout": p.stdout, "stderr": p.stderr}
        except (OSError, subprocess.TimeoutExpired) as e:
            metadata[key] = {"error": str(e)}
    (args.output / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
    p = subprocess.run([str(binary), "--probe"], env=env, text=True, capture_output=True, timeout=30)
    (args.output / "probe.txt").write_text(p.stdout + p.stderr)
    if p.returncode:
        (args.output / "summary.md").write_text(f"GPU experiment not executed. Probe exit code: {p.returncode}.\n\nNo raw benchmark samples generated. See probe.txt and environment.json.\n")
        return p.returncode
    attempts = []
    for mode in args.modes:
        combinations = [(0, 0), (0, 1), (1, 0), (1, 1)] if mode == "realtime" else [(0, 0)]
        for force, bypass in combinations:
            name = f"{mode}-f{force}-b{bypass}"
            out = args.output / name
            out.mkdir()
            cmd = [str(binary), "--run-dir", str(out.resolve()), "--mode", mode, "--trials", str(args.trials),
                   "--cta-waves", str(args.cta_waves), "--force", str(force), "--bypass", str(bypass),
                   "--heartbeat-ns", str(args.heartbeat_ns)]
            if args.graph:
                cmd += ["--graph"]
            with (out / "process.log").open("w") as log:
                try:
                    p = subprocess.run(cmd, env=env, stdout=log, stderr=subprocess.STDOUT,
                                       timeout=max(120, args.trials * 0.5 + 60))
                    rc = p.returncode
                except subprocess.TimeoutExpired:
                    rc = 124
            attempts.append({"configuration": name, "returncode": rc, "command": cmd})
            (args.output / "attempts.json").write_text(json.dumps(attempts, indent=2) + "\n")
            print(name, "exit", rc, flush=True)
            if (out / "raw.csv").is_file():
                subprocess.run([sys.executable, str(Path(__file__).with_name("summarize.py")), str(out)], check=True)
            if rc:
                # Do not continue control experiments after an unknown device/control failure.
                (args.output / "summary.md").write_text(f"Matrix stopped at {name}, exit {rc}. Inspect process.log/failure.txt; failed samples are not passes.\n")
                return rc
    (args.output / "summary.md").write_text("Matrix completed. Per-configuration summaries and unfiltered raw samples are in subdirectories; no cross-configuration causal winner is inferred automatically.\n")
    return 0

if __name__ == "__main__":
    sys.exit(main())
