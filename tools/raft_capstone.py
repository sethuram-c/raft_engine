#!/usr/bin/env python3
"""Run Phase 35 release-capstone acceptance and write a machine-readable report.

Tests run in isolated subprocesses and own temporary directories. This runner
records only exit codes, durations, counts and PASS labels; it never records
certificate contents, HMAC keys, Raft commands or server logs in its JSON.
This is an educational acceptance report, not a formal verification certificate.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys
import time


BASIC = (
    ('release_smoke', 'tests/test_release_smoke.py'),
    ('release_package', 'tests/test_release_package.py'),
    ('tls_durable_capstone', 'tests/test_capstone_integration.py'),
)
EXTENDED = (
    ('tls_regression', 'tests/test_tls_integration.py'),
    ('chaos_partition', 'tests/test_chaos_integration.py'),
    ('three_node_benchmark', 'tests/test_benchmark_integration.py'),
)


def run_stage(label: str, script: Path, binary: Path | None,
              timeout: int, cwd: Path) -> dict:
    command = [sys.executable, str(script)]
    if binary is not None:
        command.append(str(binary))
    start = time.monotonic()
    try:
        process = subprocess.run(command, cwd=cwd, capture_output=True,
                                 text=True, timeout=timeout, errors='replace')
        code = process.returncode
        timed_out = False
        lines = process.stdout.splitlines()
        passed_labels = [line[:220] for line in lines if line.startswith('PASS')]
    except subprocess.TimeoutExpired:
        code = None
        timed_out = True
        passed_labels = []
    return {
        'name': label,
        'passed': code == 0 and not timed_out,
        'exit_code': code,
        'timed_out': timed_out,
        'elapsed_ms': round((time.monotonic() - start) * 1000),
        'pass_count': len(passed_labels),
        # No raw stdout/stderr: they may contain diagnostics or local paths.
    }


def build_report(binary: Path, stages: list[dict], extended: bool) -> dict:
    return {
        'schema': 'raft-capstone-v1',
        'generated_utc': datetime.now(timezone.utc).isoformat(timespec='seconds'),
        'platform': platform.system(),
        'python_version': platform.python_version(),
        'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
        'extended': extended,
        'passed': all(stage['passed'] for stage in stages) and bool(stages),
        'stages': stages,
        'disclaimer': 'Educational integration evidence, not proof of Raft correctness or production readiness.',
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=Path('build/raft-engine-release'))
    parser.add_argument('--json-out', type=Path, default=Path('build/capstone-report.json'))
    parser.add_argument('--timeout', type=int, default=180,
                        help='per-stage limit in seconds (20..600)')
    parser.add_argument('--extended', action='store_true',
                        help='also exercise TLS regression, seeded chaos and benchmark')
    args = parser.parse_args()
    if not 20 <= args.timeout <= 600:
        parser.error('--timeout must be between 20 and 600 seconds')
    root = Path(__file__).resolve().parent.parent
    binary = (root / args.binary).resolve()
    if not binary.is_file():
        parser.error(f'compiled binary is missing: {binary}; run make release')
    plans = list(BASIC) + (list(EXTENDED) if args.extended else [])
    stages = []
    for name, relative in plans:
        script = root / relative
        # Archive verifier tests do not take a binary argument.
        selected_binary = None if name == 'release_package' else binary
        stage = run_stage(name, script, selected_binary, args.timeout, root)
        stages.append(stage)
        print(f"{'PASS' if stage['passed'] else 'FAIL'} capstone stage: {name} "
              f"({stage['elapsed_ms']} ms)", flush=True)
        if not stage['passed']:
            break  # never record later stages as passed when not executed
    report = build_report(binary, stages, args.extended)
    destination = (root / args.json_out).resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
    print(f'Capstone report: {destination}')
    return 0 if report['passed'] and len(stages) == len(plans) else 1


if __name__ == '__main__':
    raise SystemExit(main())
