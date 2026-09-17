# Schema 2 measurement summary

Run kind: performance. Rows: 1; valid application observations: 1; timing-eligible subset: 0.

All valid application observations include RM failures and ambiguous/before-RM ordering. Correctness and control outcomes are reported independently. No hardware preemption is confirmed by this summary.

- trial_state: {'complete': 1}
- application_valid: {'1': 1}
- control_status: {'SKIPPED_BY_DESIGN': 1}
- gpu_overlap: {'lifetime_overlap': 1}
- derived_ordering: {'not_applicable': 1}
- correctness_status: {'PASS': 1}
- control_target_running: {'unknown': 1}
- bg_done_before_interaction: {'0': 1}
- bg_done_before_control_observed: {'0': 1}
- bg_done_at_owner_check: {'0': 1}
- setup_status: {'BG_MAIN_OBSERVED_RESIDENCY_UNKNOWN': 1}
- timeout_rows: 0
- missing_entry_or_done_observation: 0
- failure_messages: {}

One-trial smoke: raw values only; no distribution or performance ranking.

| Metric (μs) | n | raw value |
|---|---:|---:|
| Interaction → graph entry observed, all application-valid | 1 | 1656.571 |
| Interaction → graph entry, correct application rows | 1 | 1656.571 |
| Interaction → graph entry, timing-eligible subset | 0 | N/A |
| Interaction → graph done observed, all application-valid | 1 | 1913.889 |
| Interaction → main entry observed | 1 | 1661.301 |
| Host submission | 1 | 38.161 |
| IPC request → owner receipt | 1 | 0.254 |
| IPC round trip (includes owner control when present) | 1 | 39.115 |
| Owner receipt → preparation begin | 1 | 0.309 |
| Common owner preparation | 1 | 38.076 |
| Preparation end → RM call begin | 0 | N/A |
| Preparation end → action end (no-op or active) | 1 | 0.034 |
| Action end → IPC acknowledgment | 1 | 0.442 |
| RM syscall wall time (including failures) | 0 | N/A |
| GPU graph marker interval | 1 | 257.024 |

Exact BG preempt completion, context-save duration, and BG resume latency: **unmeasured**.

Graph entry is K0 block0/thread0; main entry is the main arithmetic node. Neither is an exact first-warp timestamp. Lifetime overlap and an after-RM ordering hypothesis do not establish causation. Target residency stays unknown without another backend.

Ordering model: none; observations at/after RM are ambiguous

Paired comparisons: group-preempt-wait vs group-bound-none (matched preparation); historical preempt-wait vs none; realtime-restart vs realtime-only. Compare identical fixed work/configurations and separate diagnostic runs. One pair cannot establish a causal or stable latency benefit.

bg_calibration_after.csv: ping samples=1000, min/max RTT μs 6.031/148.631. Minimum RTT is not a full-run hard error bound.

bg_calibration_before.csv: ping samples=1000, min/max RTT μs 6.291/10.737. Minimum RTT is not a full-run hard error bound.

int_calibration_after.csv: ping samples=1000, min/max RTT μs 6.916/149.762. Minimum RTT is not a full-run hard error bound.

int_calibration_before.csv: ping samples=1000, min/max RTT μs 6.962/13.132. Minimum RTT is not a full-run hard error bound.
