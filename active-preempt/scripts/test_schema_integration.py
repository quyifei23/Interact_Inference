#!/usr/bin/env python3
"""Exercise the actual C++ CSV/journal writers. Fixtures are temporary, CPU-only."""
import csv
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from summarize import read_rows,ordering

with tempfile.TemporaryDirectory(prefix='ap-synthetic-schema-') as directory:
    p=Path(directory)
    subprocess.run([sys.argv[1],'--emit-synthetic-fixtures',directory],check=True)
    rows=read_rows(p/'raw.csv')
    assert len(rows)==4
    assert ordering(rows[0])=='before_rm' and rows[0]['T_int_main_entry_observed']=='7000'
    assert rows[1]['trial_state']=='incomplete' and rows[1]['control_status']=='CONTROL_ACCEPTED_EFFECT_UNVERIFIED'
    assert rows[1]['application_valid']=='1' and rows[1]['correctness_status']=='unknown'
    assert rows[3]['application_valid']=='0' and rows[3]['T_int_graph_done_observed']==''
    assert rows[2]['bg_present']=='0' and rows[2]['bg_correct']=='' and rows[2]['heartbeat_overflow']==''
    assert rows[2]['T_bg_done_gpu_ns']=='' and rows[2]['T_bg_main_observed']==''
    assert rows[2]['bg_done_before_control_observed']==''
    events=[json.loads(line) for line in (p/'events.jsonl').read_text().splitlines()]
    assert events[0]['syscall_return']==0 and events[0]['rm_status_valid'] is True
    assert events[1]['operation_state']=='IN_FLIGHT' and events[1]['rm_status'] is None and events[1]['call_end_ns'] is None
    subprocess.run([str(Path(sys.argv[1]).with_name('event_dump')),str(p/'events.bin'),str(p/'recovered.jsonl')],check=True)
    recovered=[json.loads(line) for line in (p/'recovered.jsonl').read_text().splitlines()]
    assert recovered==events
print('C++ CSV schema, graph-entry ordering, incomplete trials and crash journal JSON passed; no GPU exercised')
