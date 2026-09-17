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
| Interaction → graph entry observed, all application-valid | 1 | 1403.764 |
| Interaction → graph entry, correct application rows | 1 | 1403.764 |
| Interaction → graph entry, timing-eligible subset | 0 | N/A |
| Interaction → graph done observed, all application-valid | 1 | 1685.205 |
| Interaction → main entry observed | 1 | 1408.815 |
| Host submission | 1 | 40.044 |
| IPC request → owner receipt | 1 | 0.335 |
| IPC round trip (includes owner control when present) | 1 | 40.576 |
| Owner receipt → preparation begin | 1 | 0.314 |
| Common owner preparation | 1 | 39.303 |
| Preparation end → RM call begin | 0 | N/A |
| Preparation end → action end (no-op or active) | 1 | 0.036 |
| Action end → IPC acknowledgment | 1 | 0.588 |
| RM syscall wall time (including failures) | 0 | N/A |
| GPU graph marker interval | 1 | 281.600 |

Exact BG preempt completion, context-save duration, and BG resume latency: **unmeasured**.

Graph entry is K0 block0/thread0; main entry is the main arithmetic node. Neither is an exact first-warp timestamp. Lifetime overlap and an after-RM ordering hypothesis do not establish causation. Target residency stays unknown without another backend.

Ordering model: none; observations at/after RM are ambiguous

Paired comparisons: group-preempt-wait vs group-bound-none (matched preparation); historical preempt-wait vs none; realtime-restart vs realtime-only. Compare identical fixed work/configurations and separate diagnostic runs. One pair cannot establish a causal or stable latency benefit.

bg_calibration_after.csv: ping samples=1000, min/max RTT μs 7.63/165.125. Minimum RTT is not a full-run hard error bound.

bg_calibration_before.csv: ping samples=1000, min/max RTT μs 8.136/22.754. Minimum RTT is not a full-run hard error bound.

int_calibration_after.csv: ping samples=1000, min/max RTT μs 7.094/164.553. Minimum RTT is not a full-run hard error bound.

int_calibration_before.csv: ping samples=1000, min/max RTT μs 7.059/24.496. Minimum RTT is not a full-run hard error bound.
