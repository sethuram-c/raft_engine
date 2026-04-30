#!/usr/bin/env python3
"""Phase 32 Raft microbenchmark: external CLI latency, throughput and recovery.

Runs a disposable three-node --elect-fault cluster on loopback. Results are
observations on this host (including subprocess startup and TCP), NOT claims
about in-process Raft consensus latency. Never use as a security benchmark.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import importlib.util
import json
import math
import os
from pathlib import Path
import platform
import signal
import subprocess
import sys
import tempfile
import threading
import time
import csv
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('phase32_support', ROOT / 'tests' / 'test_progress_integration.py')
if SPEC is None or SPEC.loader is None:
    raise RuntimeError('missing Phase 13 integration support')
SUPPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SUPPORT)
NODES = (1, 2, 3)
SCHEMA_VERSION = 1


def percentile_nearest_rank(samples: list[float], p: float) -> float:
    """Return nearest-rank percentile, deliberately not interpolation."""
    if not samples or not 0 < p <= 100 or any(not math.isfinite(x) or x < 0 for x in samples):
        raise ValueError('need finite, nonnegative samples and percentile in (0, 100]')
    sorted_values = sorted(samples)
    return sorted_values[math.ceil(len(sorted_values) * p / 100) - 1]


def summarize_latency(samples_ms: list[float], wall_seconds: float, attempts: int) -> dict[str, Any]:
    if attempts < 0 or len(samples_ms) > attempts or not math.isfinite(wall_seconds) or wall_seconds < 0:
        raise ValueError('invalid measurement interval or attempt count')
    result = {'attempted': attempts, 'committed': len(samples_ms),
              'failed': attempts - len(samples_ms), 'wall_seconds': round(wall_seconds, 6),
              'throughput_committed_ops_s': round(len(samples_ms) / wall_seconds, 3) if wall_seconds else 0.0}
    if samples_ms:
        result['latency_ms'] = {'min': round(min(samples_ms), 3),
                                'mean': round(sum(samples_ms) / len(samples_ms), 3),
                                'p50': round(percentile_nearest_rank(samples_ms, 50), 3),
                                'p95': round(percentile_nearest_rank(samples_ms, 95), 3),
                                'p99': round(percentile_nearest_rank(samples_ms, 99), 3),
                                'max': round(max(samples_ms), 3)}
    else:
        result['latency_ms'] = None
    return result


def parse_proc_stat(line: str, ticks_per_second: int) -> float:
    """Correctly skip parenthesized Linux comm, which may contain spaces."""
    end = line.rfind(')')
    if end < 0 or ticks_per_second <= 0:
        raise ValueError('invalid proc stat')
    fields = line[end + 2:].split()
    if len(fields) < 13:
        raise ValueError('short proc stat')
    return (int(fields[11]) + int(fields[12])) * 1000.0 / ticks_per_second


def read_proc_usage(pid: int) -> dict[str, float | int] | None:
    """Return an observation, not an exact peak; absent outside Linux /proc."""
    try:
        stat = Path(f'/proc/{pid}/stat').read_text()
        status = Path(f'/proc/{pid}/status').read_text()
        rss = next(int(line.split()[1]) for line in status.splitlines() if line.startswith('VmRSS:'))
        return {'rss_kib': rss, 'cpu_ms': parse_proc_stat(stat, os.sysconf('SC_CLK_TCK'))}
    except (OSError, ValueError, IndexError, StopIteration):
        return None


def make_row(index: int, phase: str, committed: bool, elapsed_ms: float, code: int) -> dict[str, Any]:
    return {'operation': index, 'phase': phase, 'committed': int(committed),
            'latency_ms': round(elapsed_ms, 3), 'exit_code': code}


def export_results(result: dict, rows: list[dict], json_out: Path | None, csv_out: Path | None) -> None:
    for target in (json_out, csv_out):
        if target is not None:
            target.parent.mkdir(parents=True, exist_ok=True)
    if json_out is not None:
        json_out.write_text(json.dumps(result, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    if csv_out is not None:
        with csv_out.open('w', newline='', encoding='utf-8') as stream:
            writer = csv.DictWriter(stream, fieldnames=('operation', 'phase', 'committed', 'latency_ms', 'exit_code'))
            writer.writeheader()
            writer.writerows(rows)


class ResourceSampler:
    def __init__(self, processes: dict[int, subprocess.Popen]):
        self.processes = processes
        self.snapshots: dict[int, dict[str, float | int]] = {}
        self.cpu_begin: dict[int, float] = {}
        self.cpu_end: dict[int, float] = {}
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.loop, name='raft-resource-sampler', daemon=True)

    def loop(self):
        while not self.stop.is_set():
            for n, proc in list(self.processes.items()):
                observation = read_proc_usage(proc.pid)
                if observation is None:
                    continue
                old = self.snapshots.get(n)
                if old is None or observation['rss_kib'] > old['rss_kib']:
                    self.snapshots[n] = dict(observation)
                self.cpu_begin.setdefault(n, float(observation['cpu_ms']))
                self.cpu_end[n] = float(observation['cpu_ms'])
            self.stop.wait(.10)

    def start(self):
        self.thread.start()

    def finish(self) -> dict[str, dict]:
        self.stop.set()
        self.thread.join(timeout=3)
        return {str(n): {'max_sampled_rss_kib': int(v['rss_kib']),
                          'sampled_cpu_delta_ms': round(max(0., self.cpu_end[n] - self.cpu_begin[n]), 2)}
                for n, v in sorted(self.snapshots.items())}


class Benchmark:
    def __init__(self, binary: Path, writes: int, concurrency: int, warmup: int, failover: bool):
        self.binary = str(binary.resolve())
        self.writes, self.concurrency, self.warmup = writes, concurrency, warmup
        self.failover = failover
        self.ports: list[int] = []
        self.configs: list[Path] = []
        self.procs: dict[int, subprocess.Popen] = {}
        self.handles = []
        self.rules: Path | None = None
        self.known: dict[str, str] = {}
        self.rows: list[dict] = []

    def status(self, n):
        return SUPPORT.try_rpc(self.ports[n - 1], 5, SUPPORT.STATUS)

    def logs(self, n):
        return SUPPORT.try_rpc(self.ports[n - 1], 9, SUPPORT.LOG)

    def leader(self, subset=NODES):
        state = {n: self.status(n) for n in subset}
        if any(x is None for x in state.values()):
            return None
        leaders = [n for n, row in state.items() if row[2] == 2]
        terms = {row[1] for row in state.values()}
        if len(leaders) != 1 or len(terms) != 1:
            return None
        return (leaders[0], state[leaders[0]][1])

    def rule(self, n: int, blocked: list[int]):
        assert self.rules is not None
        target = self.rules / f'node-{n}.block'
        pending = self.rules / f'.node-{n}.pending'
        pending.write_text(''.join(f'{peer}\n' for peer in sorted(blocked)), encoding='ascii')
        pending.chmod(0o600)
        os.replace(pending, target)

    def write(self, leader: int, command: str, index: int, phase='measured') -> dict:
        start = time.perf_counter_ns()
        try:
            run = subprocess.run([self.binary, '--write', str(self.configs[leader % 3]),
                                  str(leader), '2500', command],
                                 capture_output=True, text=True, timeout=5)
            committed = run.returncode == 0 and 'status=COMMITTED' in run.stdout
            code = run.returncode
        except subprocess.TimeoutExpired:
            committed = False
            code = -1
        elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
        return make_row(index, phase, committed, elapsed_ms, code)

    def metrics(self, n: int) -> dict[str, int]:
        """Sample selected Phase 28 metrics; these are volatile and non-atomic."""
        sample = subprocess.run([self.binary, '--metrics', str(self.configs[n % 3]), str(n)],
                                capture_output=True, text=True, timeout=4)
        if sample.returncode:
            raise AssertionError(f'metrics unavailable for node {n}')
        result = {}
        for line in sample.stdout.splitlines():
            fields = line.split()
            if len(fields) == 2 and fields[0].startswith('raft_') and fields[1].isdigit():
                result[fields[0]] = int(fields[1])
        required = ('raft_replication_rpc_successes_total', 'raft_replication_rpc_failures_total',
                    'raft_entries_sent_total', 'raft_client_commits_total',
                    'raft_max_follower_lag', 'raft_connections_peak')
        if not all(name in result for name in required):
            raise AssertionError(f'incomplete metrics from node {n}')
        return {key: result[key] for key in required}

    def verify_values(self, nodes=NODES, timeout=18):
        def check():
            for node in nodes:
                for key, value in self.known.items():
                    try:
                        check = subprocess.run([self.binary, '--get-demo',
                                                str(self.configs[node % 3]), str(node), key],
                                               capture_output=True, text=True, timeout=3)
                    except subprocess.TimeoutExpired:
                        return None
                    if check.returncode != 0 or f'key={key} ' not in check.stdout or \
                            'found=1' not in check.stdout or f'value_hex={value.encode().hex()}' not in check.stdout:
                        return None
            return True
        SUPPORT.wait_for(check, timeout)

    def run(self) -> dict:
        with tempfile.TemporaryDirectory(prefix='raft-phase32-bench-') as tmp:
            base = Path(tmp)
            self.rules = base / 'rules'
            self.rules.mkdir(mode=0o700)
            self.ports = SUPPORT.ports_three()
            for n in NODES:
                config = base / f'{n}.conf'
                config.write_text(f'node_id={n}\n' + ''.join(
                    f'peer={i},127.0.0.1,{port}\n' for i, port in enumerate(self.ports, 1)), encoding='ascii')
                self.configs.append(config)
            started = time.monotonic()
            sampler = None
            try:
                for n in NODES:
                    stream = (base / f'node-{n}.log').open('w+')
                    self.handles.append(stream)
                    self.procs[n] = subprocess.Popen([self.binary, '--elect-fault',
                                                      str(self.configs[n - 1]), str(self.rules)],
                                                     stdout=stream, stderr=subprocess.STDOUT)
                    time.sleep(.13)
                leader, term = SUPPORT.wait_for(self.leader, 25)
                sampler = ResourceSampler(self.procs)
                sampler.start()
                for i in range(self.warmup):
                    cmd = f'SET perf_warmup_{i} value{i}'
                    row = self.write(leader, cmd, i, 'warmup')
                    if not row['committed']:
                        raise AssertionError(f'warmup write did not commit: {row}')
                before_metrics = {str(n): self.metrics(n) for n in NODES}
                # Measure the interval from first dispatch until all writes have
                # completed, including process-spawn and CLI network overhead.
                measured_start = time.perf_counter()
                def write_one(i):
                    return self.write(leader, f'SET perf_{i} value{i}', i)
                with ThreadPoolExecutor(max_workers=self.concurrency) as pool:
                    self.rows = list(pool.map(write_one, range(self.writes)))
                measured_wall = time.perf_counter() - measured_start
                for row in self.rows:
                    if row['committed']:
                        i = row['operation']
                        self.known[f'perf_{i}'] = f'value{i}'
                if any(not row['committed'] for row in self.rows):
                    raise AssertionError('measurement returned uncommitted write; inspect CSV for errors')
                self.verify_values()
                after_metrics = {str(n): self.metrics(n) for n in NODES}
                summary = summarize_latency([row['latency_ms'] for row in self.rows],
                                            measured_wall, self.writes)
                result = {'schema_version': SCHEMA_VERSION,
                          'generated_utc': datetime.now(timezone.utc).isoformat(),
                          'system': {'platform': platform.platform(), 'python': platform.python_version()},
                          'settings': {'writes': self.writes, 'concurrency': self.concurrency,
                                       'warmup': self.warmup, 'failover': self.failover},
                          'measurement': 'end-to-end external CLI spawn+TCP+majority commit',
                          'steady_state': summary,
                          'initial_leader': leader, 'initial_term': term,
                          'raft_metrics': {node: {'before': before_metrics[node],
                                                  'after': after_metrics[node],
                                                  'counter_deltas': {k: after_metrics[node][k] - before_metrics[node][k]
                                                     for k in before_metrics[node] if k.endswith('_total')}}
                                           for node in before_metrics}}
                print(f"[bench] steady: {summary['committed']}/{self.writes} committed, "
                      f"{summary['throughput_committed_ops_s']:.2f} ops/s, "
                      f"p95={summary['latency_ms']['p95']:.2f} ms", flush=True)
                if self.failover:
                    survivors = tuple(n for n in NODES if n != leader)
                    begin_fault = time.perf_counter()
                    self.rule(leader, list(survivors))
                    for n in survivors:
                        self.rule(n, [leader])
                    partition_done = time.perf_counter()
                    new_leader, new_term = SUPPORT.wait_for(lambda: self.leader(survivors), 30)
                    election_done = time.perf_counter()
                    if new_term <= term or new_leader == leader:
                        raise AssertionError('failed to observe higher-term majority leader')
                    recovery_key = 'perf_recovery'
                    row = self.write(new_leader, f'SET {recovery_key} ok', self.writes, 'recovery')
                    self.rows.append(row)
                    if not row['committed']:
                        raise AssertionError(f'recovery write not committed: {row}')
                    committed_done = time.perf_counter()
                    self.known[recovery_key] = 'ok'
                    result['failover'] = {
                        'initial_leader': leader, 'new_leader': new_leader,
                        'new_term': new_term,
                        'fault_install_ms': round((partition_done - begin_fault) * 1000, 3),
                        'majority_election_ms': round((election_done - partition_done) * 1000, 3),
                        'write_after_election_ms': row['latency_ms'],
                        'partition_to_committed_write_ms': round((committed_done - partition_done) * 1000, 3)}
                    for n in NODES:
                        self.rule(n, [])
                    SUPPORT.wait_for(self.leader, 25)
                    self.verify_values(timeout=25)
                    print(f"[bench] failover: majority election {result['failover']['majority_election_ms']:.1f} ms, "
                          f"first confirmed write {result['failover']['partition_to_committed_write_ms']:.1f} ms", flush=True)
                result['observed_duration_seconds'] = round(time.monotonic() - started, 3)
                return result
            except Exception:
                for stream in self.handles:
                    stream.flush()
                    print(f'--- {stream.name} ---\n{Path(stream.name).read_text(errors="replace")[-2000:]}',
                          file=sys.stderr)
                raise
            finally:
                # A successful return result is made available before finally.
                if sampler is not None:
                    resources = sampler.finish()
                    if 'result' in locals():
                        result['resource_samples'] = resources
                for proc in self.procs.values():
                    if proc.poll() is None:
                        proc.send_signal(signal.SIGTERM)
                        try:
                            proc.wait(timeout=4)
                        except subprocess.TimeoutExpired:
                            proc.kill()
                            proc.wait(timeout=3)
                for stream in self.handles:
                    stream.close()


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=Path('./build/raft-engine'))
    parser.add_argument('--writes', type=int, default=12, help='measured writes, 4..200')
    parser.add_argument('--concurrency', type=int, default=3, help='parallel CLI clients, 1..8')
    parser.add_argument('--warmup', type=int, default=3, help='unmeasured warmup writes, 0..20')
    parser.add_argument('--no-failover', action='store_true', help='skip leader partition and recovery measurement')
    parser.add_argument('--json-out', type=Path, help='write structured benchmark results')
    parser.add_argument('--csv-out', type=Path, help='write individual measured write samples')
    args = parser.parse_args(argv)
    if not 4 <= args.writes <= 200 or not 1 <= args.concurrency <= 8 or not 0 <= args.warmup <= 20:
        parser.error('writes must be 4..200, concurrency 1..8, warmup 0..20')
    if not args.binary.is_file():
        parser.error(f'binary not found: {args.binary}')
    benchmark = Benchmark(args.binary, args.writes, args.concurrency, args.warmup,
                          failover=not args.no_failover)
    try:
        result = benchmark.run()
    except (OSError, AssertionError, subprocess.TimeoutExpired) as exc:
        print(f'BENCHMARK FAILED: {exc}', file=sys.stderr)
        return 1
    export_results(result, benchmark.rows, args.json_out, args.csv_out)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
