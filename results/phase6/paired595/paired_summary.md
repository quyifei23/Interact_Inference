# Phase 6 paired observations

Planned 10 pairs / 20 runs; started 20; complete runs 20; complete pairs 10.
Active slots reserved 10; known PREEMPT attempts 10; unresolved slots 0.
Stopped reason: none.
Started pairs 10; missing entry/done among started runs 0/0; incomplete trials 0; failed/unconfirmed runs 0; control errors 0; timeout runs 0.
Mechanism timing-eligible samples 0 (additional subset only; all application intervals remain in the primary table).

Δ = control − treatment. Positive means the treatment observation was earlier. All known application intervals are retained, including RM failures, ambiguous/before-RM ordering, late BG and incorrect outputs. Missing intervals remain unknown; configuration mismatches cannot form a matched delta. This is a small fixed batch, with no p99/SLA or adaptive stopping claim.

| Pair | Order | Delay μs | C entry μs | T entry μs | Δ entry μs | C done μs | T done μs | Δ done μs | T ordering | Complete/correct |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|---|
| 0 | TC | 3255 | 1132.679 | 408.592 | 724.087 | 1387.731 | 666.554 | 721.177 | ordering_ambiguous | True |
| 1 | TC | 2344 | 2037.684 | 403.284 | 1634.400 | 2289.445 | 675.729 | 1613.716 | ordering_ambiguous | True |
| 2 | CT | 4102 | 280.412 | 280.860 | -0.448 | 537.595 | 535.993 | 1.602 | ordering_ambiguous | True |
| 3 | CT | 4562 | 1918.537 | 400.796 | 1517.741 | 2174.624 | 658.134 | 1516.490 | ordering_ambiguous | True |
| 4 | CT | 3228 | 1156.234 | 406.940 | 749.294 | 1413.622 | 667.017 | 746.605 | ordering_ambiguous | True |
| 5 | TC | 4825 | 1656.571 | 422.123 | 1234.448 | 1913.889 | 688.843 | 1225.046 | ordering_ambiguous | True |
| 6 | TC | 3130 | 1255.725 | 409.687 | 846.038 | 1512.696 | 666.499 | 846.197 | ordering_ambiguous | True |
| 7 | CT | 1855 | 433.622 | 427.525 | 6.097 | 690.868 | 689.520 | 1.348 | ordering_ambiguous | True |
| 8 | CT | 3661 | 723.341 | 418.627 | 304.714 | 982.310 | 678.561 | 303.749 | ordering_ambiguous | True |
| 9 | TC | 1875 | 413.596 | 407.852 | 5.744 | 671.240 | 666.941 | 4.299 | ordering_ambiguous | True |

entry: control median 1144.457 μs (n=10), treatment median 408.222 μs (n=10).
Paired Δ: n=10, median 736.691, mean 702.212, range [-0.448, 1634.400] μs; improved/worse/equal=9/1/0.
Complete/correct subset: n=10, median Δ 736.691 μs.
CT: n=5, median Δ 304.714 μs, all deltas ns=[-448, 1517741, 749294, 6097, 304714].
TC: n=5, median Δ 846.038 μs, all deltas ns=[724087, 1634400, 1234448, 846038, 5744].

done: control median 1400.677 μs (n=10), treatment median 666.979 μs (n=10).
Paired Δ: n=10, median 733.891, mean 698.023, range [1.348, 1613.716] μs; improved/worse/equal=10/0/0.
Complete/correct subset: n=10, median Δ 733.891 μs.
CT: n=5, median Δ 303.749 μs, all deltas ns=[1602, 1516490, 746605, 1348, 303749].
TC: n=5, median Δ 846.197 μs, all deltas ns=[721177, 1613716, 1225046, 846197, 4299].

Independent dimensions:

{
  "control": {
    "run_state": {
      "COMPLETE": 10
    },
    "application_valid": {
      "1": 10
    },
    "trial_state": {
      "complete": 10
    },
    "control_status": {
      "SKIPPED_BY_DESIGN": 10
    },
    "ordering_relative_to_rm": {
      "not_applicable": 10
    },
    "gpu_overlap": {
      "lifetime_overlap": 10
    },
    "correctness_status": {
      "PASS": 10
    },
    "setup_status": {
      "BG_MAIN_OBSERVED_RESIDENCY_UNKNOWN": 10
    },
    "cleanup_verified": {
      "True": 10
    }
  },
  "treatment": {
    "run_state": {
      "COMPLETE": 10
    },
    "application_valid": {
      "1": 10
    },
    "trial_state": {
      "complete": 10
    },
    "control_status": {
      "CONTROL_ACCEPTED_EFFECT_UNVERIFIED": 10
    },
    "ordering_relative_to_rm": {
      "ordering_ambiguous": 10
    },
    "gpu_overlap": {
      "lifetime_overlap": 10
    },
    "correctness_status": {
      "PASS": 10
    },
    "setup_status": {
      "BG_MAIN_OBSERVED_RESIDENCY_UNKNOWN": 10
    },
    "cleanup_verified": {
      "True": 10
    }
  }
}

Timing decomposition (all returned/observed rows, μs; no-op has no RM syscall interval):

| Condition | Metric | n | Median | Min | Max |
|---|---|---:|---:|---:|---:|
| control | submit_ns | 10 | 39.920 | 23.118 | 41.334 |
| control | ipc_request_ns | 10 | 0.268 | 0.115 | 0.355 |
| control | receive_to_prepare_ns | 10 | 0.310 | 0.219 | 0.589 |
| control | prepare_ns | 10 | 37.927 | 33.481 | 47.411 |
| control | prepare_to_rm_ns | 0 | unknown | unknown | unknown |
| control | rm_wall_ns | 0 | unknown | unknown | unknown |
| control | prepare_to_action_end_ns | 10 | 0.035 | 0.034 | 0.045 |
| control | ipc_roundtrip_ns | 10 | 39.066 | 34.502 | 48.761 |
| control | trigger_lateness_ns | 10 | 0.079 | 0.056 | 0.096 |
| treatment | submit_ns | 10 | 39.697 | 24.699 | 41.599 |
| treatment | ipc_request_ns | 10 | 0.307 | 0.141 | 0.367 |
| treatment | receive_to_prepare_ns | 10 | 0.469 | 0.291 | 0.676 |
| treatment | prepare_ns | 10 | 38.215 | 31.640 | 47.950 |
| treatment | prepare_to_rm_ns | 10 | 1.468 | 0.930 | 1.853 |
| treatment | rm_wall_ns | 10 | 472.165 | 297.329 | 479.718 |
| treatment | prepare_to_action_end_ns | 10 | 473.404 | 298.854 | 481.613 |
| treatment | ipc_roundtrip_ns | 10 | 512.866 | 335.795 | 530.908 |
| treatment | trigger_lateness_ns | 10 | 0.085 | 0.057 | 0.090 |

Preparation includes lock acquisition, current environment/FD validation, full registry/snapshot and exact GET_INFO checks, and metadata bookkeeping. It is not solely registry scanning.

Host-observed entry/done are application observations. CPU/GPU clocks are not directly subtracted; at/after-RM observation remains ambiguous. No ordering model was enabled. Exact BG preempt completion, context-save duration and BG resume latency remain unmeasured. A lower paired latency does not by itself establish hardware preemption causality, sustained suspension, or a scheduler guarantee.

Final bit-exact outputs and same-context short reuse test final correctness/usability, not absence of replay or instruction-level context save. One captured TSG is not proof of complete CUDA context resource coverage.
