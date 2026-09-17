Phase 7: DIAGNOSTIC_ASSOCIATION_OBSERVED (scoped association).

D0/D1: one diagnostic trial each; project GET_INFO=4, PREEMPT=1, other scheduling controls=0. D1 syscall=0, errno=0, NV_STATUS=0. Both references, BG same-context/stream reuse and cleanup passed.

Nsight2024.6.2: 18 context-switch records per run; D1 BG SAVE_END and INT RESTORE_START plus INT kernel start inside the PREEMPT NVTX envelope; BG RESTORE_START and original kernel completion followed. No native switch-ID/TSG-ID equality, exact hardware save/resume duration or exclusive causality claimed. RAW ordering stays ambiguous; drop count unknown; driver-version warning retained. Phase6 unchanged.

550/595 final CTest=10/10 each; diagnostic Python=8/8. One earlier 550 timeout-child regression failure is retained and unexplained; unchanged isolated/full reruns passed. No more GPU samples were taken for this test failure.

See ../../docs/phase7_diagnostic.md and ../../docs/prototype_final.md. freeze.json records code, binary and evidence identities; no commit/push/tag was created. Original opaque Nsight reports remain local/private; analysis/raw_records.jsonl is the selected raw-row export.
