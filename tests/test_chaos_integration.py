#!/usr/bin/env python3
"""Phase 31 fast seeded smoke: real three-node partition, repair and convergence."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
binary = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / 'build' / 'raft-engine').resolve()
with tempfile.TemporaryDirectory(prefix='raft-phase31-test-') as temp:
    target = Path(temp)
    command = [sys.executable, str(ROOT / 'tools' / 'raft_chaos.py'),
               '--binary', str(binary), '--seed', '31', '--rounds', '1',
               '--plan-out', str(target / 'schedule.json'),
               '--trace', str(target / 'events.jsonl')]
    result = subprocess.run(command, timeout=125, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        print(result.stdout, file=sys.stderr)
        raise SystemExit(result.returncode)
    events = (target / 'events.jsonl').read_text()
    assert '"event": "success"' in events, events
    assert '"event": "isolated_write_timeout"' in events, events
    assert '"event": "healed_and_converged"' in events, events
    assert '"event": "majority_write_committed"' in events, events
    print(result.stdout)
    print('PASS Phase 31 seeded chaos integration (seed=31, rounds=1)')
