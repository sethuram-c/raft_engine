#!/usr/bin/env python3
"""Phase 30: rootless, reproducible outgoing-peer partitions and Raft safety.

Nodes run with --elect-fault and per-node atomically replaced *.block files.
The client diagnostics bypass faults by design; only internal peer RPCs drop.
No firewall rules, elevated privileges or destructive network changes.
"""
from __future__ import annotations
import importlib.util
from pathlib import Path
import os
import signal
import subprocess
import sys
import tempfile
import time

spec = importlib.util.spec_from_file_location(
    'progress_support', Path(__file__).with_name('test_progress_integration.py'))
assert spec and spec.loader
support = importlib.util.module_from_spec(spec)
spec.loader.exec_module(support)


def call(binary, *args, timeout=6):
    return subprocess.run([binary, *map(str, args)], capture_output=True,
                          text=True, timeout=timeout)


def main(binary):
    binary = str(Path(binary).resolve())
    with tempfile.TemporaryDirectory(prefix='raft-phase30-') as temp:
        root = Path(temp)
        rules = root / 'faults'
        rules.mkdir(mode=0o700)
        ports = support.ports_three()
        configs = []
        for n in (1, 2, 3):
            file = root / f'node{n}.conf'
            file.write_text(f'node_id={n}\n' + ''.join(
                f'peer={i},127.0.0.1,{port}\n' for i, port in enumerate(ports, 1)))
            configs.append(file)
        logs = []
        procs = {}
        counts = {n: 0 for n in (1, 2, 3)}

        def start(n):
            counts[n] += 1
            out = open(root / f'node-{n}-{counts[n]}.log', 'w+')
            logs.append(out)
            procs[n] = subprocess.Popen(
                [binary, '--elect-fault', str(configs[n - 1]), str(rules)],
                stdout=out, stderr=subprocess.STDOUT)

        def stop(n):
            p = procs.get(n)
            if p and p.poll() is None:
                p.send_signal(signal.SIGTERM)
                try: p.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    p.kill(); p.wait(timeout=3)

        def status(n):
            return support.try_rpc(ports[n-1], 5, support.STATUS)

        def log(n):
            return support.try_rpc(ports[n-1], 9, support.LOG)

        def write(n, command, timeout=1800):
            return call(binary, '--write', configs[n % 3], n, timeout, command, timeout=7)

        def get(n, key):
            return call(binary, '--get-demo', configs[n % 3], n, key)

        def rule(n, blocked):
            """Never expose a half-written policy to an election thread."""
            pending = rules / f'.node-{n}.pending'
            pending.write_text(''.join(f'{p}\n' for p in sorted(blocked)))
            pending.chmod(0o600)
            os.replace(pending, rules / f'node-{n}.block')

        def all_up_same_leader():
            samples = {n: status(n) for n in (1, 2, 3)}
            if any(s is None for s in samples.values()):
                return None
            leaders = [n for n, s in samples.items() if s[2] == 2]
            terms = {s[1] for s in samples.values()}
            return (leaders[0], terms.pop()) if len(leaders) == len(terms) == 1 else None

        try:
            for n in (1, 2, 3):
                start(n)
                time.sleep(.13)
            leader, first_term = support.wait_for(all_up_same_leader, 22)
            baseline = write(leader, 'SET baseline yes')
            assert baseline.returncode == 0 and 'COMMITTED' in baseline.stdout, baseline
            support.wait_for(lambda: all((x := log(n)) is not None and x[2] >= 1
                                         for n in (1, 2, 3)), 12)
            print('PASS fault: healthy three-node leader commits baseline entry')

            # A bidirectional partition with old leader isolated, survivors connected.
            survivors = [n for n in (1, 2, 3) if n != leader]
            rule(leader, survivors)
            for n in survivors:
                rule(n, [leader])
            print(f'Partition: isolated={leader}, majority={survivors}')

            # The isolated leader remains reachable by external clients. Its
            # accepted entry MUST NOT acquire a majority or alter applied state.
            old = write(leader, 'SET isolated never', timeout=550)
            assert old.returncode == 3 and 'TIMEOUT' in old.stdout, old
            local = log(leader)
            assert local is not None and local[0] >= 2 and local[2] == 1, local
            assert 'found=0' in get(leader, 'isolated').stdout
            print('PASS fault: isolated leader returns TIMEOUT; uncommitted value stays invisible')

            def majority_leader():
                v = {n: status(n) for n in survivors}
                if any(x is None for x in v.values()): return None
                elected = [n for n, x in v.items() if x[2] == 2]
                if len(elected) != 1 or len({x[1] for x in v.values()}) != 1:
                    return None
                return (elected[0], v[elected[0]][1])
            new_leader, new_term = support.wait_for(majority_leader, 24)
            assert new_term > first_term, (new_term, first_term)
            # An isolated old-term leader can temporarily remain leader locally,
            # but only the majority can elect the leader in the new term.
            for n in survivors:
                s = status(n)
                assert s and s[1] == new_term
            print('PASS fault: partition majority elects one higher-term leader')

            majority_write = write(new_leader, 'SET majority wins', timeout=2400)
            assert majority_write.returncode == 0 and 'COMMITTED' in majority_write.stdout, majority_write
            support.wait_for(lambda: all((x := log(n)) is not None and x[2] >= 2
                                         for n in survivors), 10)
            assert all('found=1' in get(n, 'majority').stdout for n in survivors)
            assert all('found=0' in get(n, 'isolated').stdout for n in survivors)
            print('PASS fault: two-node majority commits; isolated suffix cannot be committed')

            # Heal both directions. Higher term forces the old leader to step
            # down; AppendEntries repairs its conflicting uncommitted suffix.
            for n in (1, 2, 3): rule(n, [])
            support.wait_for(all_up_same_leader, 22)
            support.wait_for(lambda: all((x := log(n)) is not None and x[2] >= 2
                                         for n in (1, 2, 3)), 18)
            support.wait_for(lambda: all('found=1' in get(n,'majority').stdout and
                                         'found=0' in get(n,'isolated').stdout
                                         for n in (1, 2, 3)), 12)
            terms = {status(n)[1] for n in (1, 2, 3)}
            assert len(terms) == 1
            print('PASS fault: healed former leader steps down; majority history wins; states converge')

            # A restarted follower with no state must be able to catch up after
            # the partition heals; memory-only mode intentionally loses its log.
            follower = next(n for n in (1, 2, 3) if status(n)[2] != 2)
            stop(follower)
            start(follower)
            support.wait_for(lambda: (x := log(follower)) is not None and x[2] >= 2,
                             20)
            support.wait_for(lambda: 'found=1' in get(follower,'majority').stdout, 12)
            assert 'found=0' in get(follower,'isolated').stdout
            print('PASS fault: restarted follower catches up with committed majority history')

            # Invalid path/permissions must reject fault mode before serving.
            open_directory = root / 'unsafe'
            open_directory.mkdir(mode=0o755)
            bad = call(binary, '--elect-fault', configs[0], open_directory, timeout=3)
            assert bad.returncode != 0
            print('PASS fault: unsafe rule directory rejected')
        except Exception:
            for stream in logs:
                stream.flush()
                print(f'--- {stream.name} ---', file=sys.stderr)
                print(Path(stream.name).read_text()[-5000:], file=sys.stderr)
            raise
        finally:
            for n in list(procs): stop(n)
            for stream in logs: stream.close()
    print('PASS Phase 30 partition and recovery integration')


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else './build/raft-engine')
