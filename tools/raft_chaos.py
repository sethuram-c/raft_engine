#!/usr/bin/env python3
"""Phase 31: seeded, rootless, repeatable Raft chaos *schedules*.

Only test-only --elect-fault nodes are launched. A seed reproduces the action
plan, not the OS/network scheduling or the exact sequence of Raft elections.
Runs entirely on localhost and changes no firewall rules.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import importlib.util
import json
import os
from pathlib import Path
import random
import re
import signal
import subprocess
import sys
import tempfile
import time
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    'raft_phase31_support', ROOT / 'tests' / 'test_progress_integration.py')
if SPEC is None or SPEC.loader is None:
    raise RuntimeError('Phase 13 integration helpers are missing')
SUPPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SUPPORT)

PLAN_VERSION = 1
NODES = (1, 2, 3)
MAX_ROUNDS = 8
MAX_WRITES = 5


def create_plan(seed: int, rounds: int) -> dict[str, Any]:
    if not 0 <= seed <= 2**32 - 1 or not 1 <= rounds <= MAX_ROUNDS:
        raise ValueError('seed must be uint32, rounds must be 1..8')
    rng = random.Random(seed)
    stages = []
    for r in range(rounds):
        # Seed drives workload content and restart decisions. The isolated
        # leader is chosen from *observed runtime state*, never guessed.
        count = rng.randint(2, MAX_WRITES)
        operations = [
            {'key': f'chaos{r}_{k}', 'value': f'v{rng.getrandbits(32):08x}'}
            for k in range(count)
        ]
        stages.append({'round': r, 'operations': operations,
                       'delete_index': rng.randrange(count),
                       'restart_follower': bool(rng.getrandbits(1))})
    return {'version': PLAN_VERSION, 'seed': seed, 'rounds': stages}


def validate_plan(plan: Any) -> dict[str, Any]:
    if not isinstance(plan, dict) or set(plan) != {'version', 'seed', 'rounds'}:
        raise ValueError('invalid plan envelope')
    seed, rounds = plan['seed'], plan['rounds']
    if (type(seed) is not int or not 0 <= seed <= 2**32-1 or
            plan['version'] != PLAN_VERSION or type(plan['version']) is not int or
            not isinstance(rounds, list) or not 1 <= len(rounds) <= MAX_ROUNDS):
        raise ValueError('unsupported plan version, seed, or round count')
    for i, step in enumerate(rounds):
        if (not isinstance(step, dict) or
                set(step) != {'round', 'operations', 'delete_index', 'restart_follower'} or
                type(step['round']) is not int or step['round'] != i or
                type(step['restart_follower']) is not bool or
                type(step['delete_index']) is not int or
                not 0 <= step['delete_index'] < len(step['operations']) or
                not isinstance(step['operations'], list) or
                not 2 <= len(step['operations']) <= MAX_WRITES):
            raise ValueError('invalid round schedule')
        for j, operation in enumerate(step['operations']):
            if (not isinstance(operation, dict) or
                    set(operation) != {'key', 'value'} or
                    operation['key'] != f'chaos{i}_{j}' or
                    not isinstance(operation['value'], str) or
                    re.fullmatch(r'v[0-9a-f]{8}', operation['value']) is None):
                raise ValueError('invalid write operation')
    return plan


def check_observation(status: dict[int, tuple], logs: dict[int, tuple]) -> None:
    """Local and sampled cross-node safety sanity checks.

    Only simultaneous leaders *observed with the same term* are suspicious;
    differing terms are allowed during partitions. Samples are sequential,
    so the check is empirical, not a linearizable global snapshot.
    """
    by_term: dict[int, int] = {}
    for n, sample in status.items():
        if len(sample) != 4 or sample[0] != n or sample[2] not in (0, 1, 2):
            raise AssertionError(f'invalid STATUS node={n}: {sample}')
        if sample[2] == 2:
            previous = by_term.setdefault(sample[1], n)
            if previous != n:
                raise AssertionError(f'two sampled leaders in term {sample[1]}: {previous},{n}')
    for n, row in logs.items():
        if len(row) != 3 or not (0 <= row[2] <= row[0]):
            raise AssertionError(f'commit index exceeds log index on {n}: {row}')


def matching_value(stdout: str, key: str, expected: str | None) -> bool:
    if f'key={key} ' not in stdout:
        return False
    if expected is None:
        return 'found=0' in stdout
    match = re.search(r'\bfound=1\b.*\bvalue_hex=([0-9a-fA-F]*)', stdout)
    return bool(match and match.group(1).lower() == expected.encode().hex())


class ChaosRun:
    def __init__(self, binary: Path, plan: dict, trace: Path | None):
        self.binary = str(binary.resolve())
        self.plan = validate_plan(plan)
        self.trace_path = trace
        self.started_at = time.monotonic()
        self.events: list[dict[str, Any]] = []
        self.temp = None
        self.root = None
        self.rules = None
        self.ports = []
        self.configs = []
        self.procs: dict[int, subprocess.Popen] = {}
        self.handles = []
        self.starts = {n: 0 for n in NODES}
        self.expected: dict[str, str | None] = {}
        self.commits = 0

    def record(self, event: str, **data: Any):
        row = {'elapsed_ms': round((time.monotonic()-self.started_at)*1000),
               'event': event, **data}
        self.events.append(row)
        print(f'[chaos] {event}: {data}', flush=True)
        if self.trace_path is not None:
            self.trace_path.parent.mkdir(parents=True, exist_ok=True)
            with self.trace_path.open('a', encoding='utf-8') as f:
                f.write(json.dumps(row, sort_keys=True) + '\n')

    def start(self, n: int):
        self.starts[n] += 1
        stream = (self.root / f'node-{n}-{self.starts[n]}.log').open('w+')
        self.handles.append(stream)
        self.procs[n] = subprocess.Popen(
            [self.binary, '--elect-fault', str(self.configs[n-1]), str(self.rules)],
            stdout=stream, stderr=subprocess.STDOUT)
        self.record('start_node', node=n, instance=self.starts[n])

    def stop(self, n: int):
        proc = self.procs.get(n)
        if proc is not None and proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=4)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=4)
        self.record('stop_node', node=n)

    def rule(self, n: int, blocked: list[int]):
        path = self.rules / f'node-{n}.block'
        pending = self.rules / f'.node-{n}.pending'
        # Atomic write: readers never see a partially updated rule.
        pending.write_text(''.join(f'{p}\n' for p in sorted(blocked)), encoding='ascii')
        pending.chmod(0o600)
        os.replace(pending, path)
        self.record('outbound_block', node=n, peers=sorted(blocked))

    def status(self, n: int):
        return SUPPORT.try_rpc(self.ports[n-1], 5, SUPPORT.STATUS)

    def log(self, n: int):
        return SUPPORT.try_rpc(self.ports[n-1], 9, SUPPORT.LOG)

    def leader(self, nodes=NODES):
        data = {n: self.status(n) for n in nodes}
        if any(v is None for v in data.values()):
            return None
        logs = {n: self.log(n) for n in nodes}
        check_observation(data, {n: x for n, x in logs.items() if x is not None})
        leaders = [n for n, row in data.items() if row[2] == 2]
        if len(leaders) != 1 or len({row[1] for row in data.values()}) != 1:
            return None
        return (leaders[0], data[leaders[0]][1])

    def cli(self, *args, timeout=8):
        return subprocess.run([self.binary, *map(str, args)],
                              capture_output=True, text=True, timeout=timeout)

    def write(self, leader: int, cmd: str, ms: int=2500):
        # The caller uses a configured endpoint and the leader ID explicitly.
        return self.cli('--write', self.configs[leader % 3], leader, ms, cmd,
                        timeout=ms/1000 + 5)

    def get(self, node: int, key: str):
        return self.cli('--get-demo', self.configs[node % 3], node, key, timeout=5)

    def verify(self, nodes=NODES, timeout=16):
        def check():
            status = {n: self.status(n) for n in nodes}
            logs = {n: self.log(n) for n in nodes}
            if any(v is None for v in status.values()) or any(v is None for v in logs.values()):
                return None
            check_observation(status, logs)
            if any(row[2] < self.commits for row in logs.values()):
                return None
            for n in nodes:
                for key, value in self.expected.items():
                    result = self.get(n, key)
                    if result.returncode != 0 or not matching_value(result.stdout, key, value):
                        return None
            return True
        SUPPORT.wait_for(check, timeout)
        self.record('verified_committed_values', nodes=list(nodes),
                    checked_keys=len(self.expected), confirmed_writes=self.commits)

    def run(self):
        with tempfile.TemporaryDirectory(prefix='raft-phase31-') as temp:
            self.root = Path(temp)
            self.rules = self.root / 'rules'
            self.rules.mkdir(mode=0o700)
            self.ports = SUPPORT.ports_three()
            for n in NODES:
                path = self.root / f'node{n}.conf'
                path.write_text(f'node_id={n}\n' + ''.join(
                    f'peer={p},127.0.0.1,{port}\n'
                    for p, port in enumerate(self.ports, 1)), encoding='ascii')
                self.configs.append(path)
            self.record('schedule', plan=self.plan)
            try:
                for n in NODES:
                    self.start(n)
                    time.sleep(.13)
                for step in self.plan['rounds']:
                    r = step['round']
                    leader, term = SUPPORT.wait_for(self.leader, 25)
                    self.record('round_begin', round=r, leader=leader, term=term)
                    # Concurrent, independent SET requests exercise TCP workers
                    # and commit tracking without relying on ordering of responses.
                    with ThreadPoolExecutor(max_workers=4) as workers:
                        responses = list(workers.map(
                            lambda op: self.write(leader, f"SET {op['key']} {op['value']}"),
                            step['operations']))
                    for operation, response in zip(step['operations'], responses):
                        key, value = operation['key'], operation['value']
                        if response.returncode != 0 or 'COMMITTED' not in response.stdout:
                            raise AssertionError(f'parallel write failed: {response}')
                        self.expected[key] = value
                        self.commits += 1
                        self.record('write_committed', round=r, key=key,
                                    response=response.stdout.strip())
                    delete_key = step['operations'][step['delete_index']]['key']
                    deletion = self.write(leader, f'DEL {delete_key}')
                    if deletion.returncode != 0 or 'COMMITTED' not in deletion.stdout:
                        raise AssertionError(f'delete not committed: {deletion}')
                    self.expected[delete_key] = None
                    self.commits += 1
                    self.record('delete_committed', round=r, key=delete_key)
                    self.verify()
                    isolated = leader
                    survivors = [n for n in NODES if n != isolated]
                    self.rule(isolated, survivors)
                    for n in survivors:
                        self.rule(n, [isolated])
                    self.record('partition', round=r, isolated=isolated,
                                majority=survivors)
                    unavailable_key = f'chaos{r}_isolated'
                    reply = self.write(isolated, f'SET {unavailable_key} never', 600)
                    if reply.returncode != 3 or 'TIMEOUT' not in reply.stdout:
                        raise AssertionError(f'isolation must not confirm write: {reply}')
                    old_log = self.log(isolated)
                    if not old_log or old_log[2] != self.commits:
                        raise AssertionError(f'isolated node committed without majority: {old_log}')
                    if not matching_value(self.get(isolated, unavailable_key).stdout,
                                          unavailable_key, None):
                        raise AssertionError('uncommitted command visible on isolated node')
                    self.record('isolated_write_timeout', round=r, key=unavailable_key)
                    new_leader, new_term = SUPPORT.wait_for(
                        lambda: self.leader(tuple(survivors)), 25)
                    if new_term <= term:
                        raise AssertionError('new leader not in higher term')
                    important_key = f'chaos{r}_majority'
                    committed = self.write(new_leader, f'SET {important_key} safe')
                    if committed.returncode != 0 or 'COMMITTED' not in committed.stdout:
                        raise AssertionError(f'majority write failed: {committed}')
                    self.expected[important_key] = 'safe'
                    self.commits += 1
                    self.verify(tuple(survivors))
                    self.record('majority_write_committed', round=r,
                                leader=new_leader, term=new_term)
                    for n in NODES:
                        self.rule(n, [])
                    SUPPORT.wait_for(self.leader, 25)
                    self.verify()
                    for n in NODES:
                        if not matching_value(self.get(n, unavailable_key).stdout,
                                              unavailable_key, None):
                            raise AssertionError('uncommitted isolated value survived healing')
                    self.record('healed_and_converged', round=r)
                    if step['restart_follower']:
                        active_leader, _ = SUPPORT.wait_for(self.leader, 12)
                        victim = next(n for n in NODES if n != active_leader)
                        self.stop(victim)
                        self.start(victim)
                        self.verify(timeout=22)
                        self.record('restart_caught_up', round=r, node=victim)
                self.record('success', rounds=len(self.plan['rounds']),
                            confirmed_writes=self.commits)
            except Exception:
                for stream in self.handles:
                    stream.flush()
                    tail = Path(stream.name).read_text(errors='replace')[-3000:]
                    print(f'--- {stream.name} ---\n{tail}', file=sys.stderr)
                self.record('failure')
                raise
            finally:
                for n in list(self.procs):
                    self.stop(n)
                for stream in self.handles:
                    stream.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='./build/raft-engine')
    parser.add_argument('--seed', type=int, default=31)
    parser.add_argument('--rounds', type=int, default=2)
    parser.add_argument('--plan-out', type=Path,
                        help='write a portable JSON action plan (no ports/timing)')
    parser.add_argument('--replay-plan', type=Path, help='read an earlier plan JSON')
    parser.add_argument('--trace', type=Path, help='save ordered JSONL observations')
    parser.add_argument('--plan-only', action='store_true', help='generate/verify without launching nodes')
    args = parser.parse_args(argv)
    try:
        plan = validate_plan(json.loads(args.replay_plan.read_text())) if args.replay_plan else \
               create_plan(args.seed, args.rounds)
        if args.plan_out:
            args.plan_out.parent.mkdir(parents=True, exist_ok=True)
            args.plan_out.write_text(json.dumps(plan, indent=2, sort_keys=True) + '\n')
        if args.plan_only:
            print(json.dumps(plan, indent=2, sort_keys=True))
            return 0
        binary = Path(args.binary)
        if not binary.is_file():
            parser.error(f'binary not found: {binary}')
        if args.trace and args.trace.exists():
            args.trace.unlink()  # never merge events from different runs
        ChaosRun(binary, plan, args.trace).run()
        return 0
    except (ValueError, OSError, AssertionError, subprocess.TimeoutExpired) as error:
        print(f'CHAOS FAIL: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
