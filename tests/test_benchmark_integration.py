#!/usr/bin/env python3
"""End-to-end Phase 32 three-node performance smoke (not a performance threshold)."""
import csv
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / 'build' / 'raft-engine').resolve()
with tempfile.TemporaryDirectory(prefix='raft-phase32-integration-') as tmp:
    base = Path(tmp)
    cmd = [sys.executable, str(ROOT / 'tools' / 'raft_bench.py'), '--binary', str(BINARY),
           '--writes', '8', '--concurrency', '2', '--warmup', '2',
           '--json-out', str(base / 'result.json'), '--csv-out', str(base / 'samples.csv')]
    run = subprocess.run(cmd, text=True, capture_output=True, timeout=140)
    if run.returncode:
        print(run.stdout, file=sys.stderr)
        print(run.stderr, file=sys.stderr)
        raise SystemExit(run.returncode)
    result = json.loads((base / 'result.json').read_text())
    summary = result['steady_state']
    assert result['schema_version'] == 1 and summary['committed'] == 8, result
    assert summary['failed'] == 0 and summary['throughput_committed_ops_s'] > 0, result
    assert 0 < summary['latency_ms']['p50'] <= summary['latency_ms']['p95'] <= summary['latency_ms']['p99'], result
    assert result['failover']['new_term'] > result['initial_term'], result
    assert result['failover']['partition_to_committed_write_ms'] > 0, result
    assert len(result['resource_samples']) == 3, result
    with (base / 'samples.csv').open() as source:
        rows = list(csv.DictReader(source))
    assert len(rows) == 9 and all(int(row['committed']) for row in rows), rows
    print('PASS Phase 32 real three-node write latency, throughput, resource samples and partition recovery')
    print(f"  p95={summary['latency_ms']['p95']} ms, throughput={summary['throughput_committed_ops_s']} ops/s")
    print(f"  partition-to-confirmed-write={result['failover']['partition_to_committed_write_ms']} ms")
