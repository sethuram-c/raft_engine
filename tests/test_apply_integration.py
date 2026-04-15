#!/usr/bin/env python3
"""Phase 15: commit-gated deterministic state-machine application in three processes."""
from __future__ import annotations

import importlib.util
import signal
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    'progress_support', Path(__file__).with_name('test_progress_integration.py'))
assert spec is not None and spec.loader is not None
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
APPLY = struct.Struct('!QQIQ')
READ_HEADER = struct.Struct('!BQH')


def main(binary: str) -> None:
    binary = str(Path(binary).resolve())
    with tempfile.TemporaryDirectory(prefix='raft-phase15-') as directory:
        base = Path(directory)
        ports = module.ports_three()
        configs = []
        for n in (1, 2, 3):
            cfg = base / f'node{n}.conf'
            cfg.write_text(f'node_id={n}\n' + ''.join(
                f'peer={i},127.0.0.1,{port}\n' for i, port in enumerate(ports, 1)))
            configs.append(cfg)
        procs: dict[int, subprocess.Popen] = {}
        logs = []

        def start(n):
            f = open(base / f'node{n}-{len(logs)}.log', 'w+')
            logs.append(f)
            procs[n] = subprocess.Popen([binary, '--elect', str(configs[n-1])],
                                        stdout=f, stderr=subprocess.STDOUT)

        def stop(n):
            p = procs.get(n)
            if p is None or p.poll() is not None:
                return
            p.send_signal(signal.SIGTERM)
            try:
                p.wait(timeout=4)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait(timeout=2)

        def status(n):
            return module.try_rpc(ports[n-1], 5, module.STATUS)

        def log(n):
            return module.try_rpc(ports[n-1], 9, module.LOG)

        def applied(n):
            return module.try_rpc(ports[n-1], 13, APPLY)

        def read(n, key):
            typ, payload = module.rpc(ports[n-1], 15, key.encode())
            assert typ == 16 and len(payload) >= READ_HEADER.size
            found, last_applied, length = READ_HEADER.unpack(payload[:READ_HEADER.size])
            assert found in (0, 1) and len(payload) == READ_HEADER.size + length
            assert found or length == 0
            return found, last_applied, payload[READ_HEADER.size:]

        def propose(n, command):
            typ, payload = module.rpc(ports[n-1], 11, command)
            assert typ == 12 and len(payload) == module.PROPOSE_REPLY.size
            return module.PROPOSE_REPLY.unpack(payload)

        def stable():
            states = {n: status(n) for n in (1, 2, 3)}
            if any(s is None for s in states.values()):
                return None
            leaders = [n for n, s in states.items() if s[2] == 2]
            terms = {s[1] for s in states.values()}
            return (leaders[0], terms.pop()) if len(leaders) == len(terms) == 1 else None

        try:
            for n in (1, 2, 3):
                start(n)
            leader, term = module.wait_for(stable, duration=10)
            follower, other = [n for n in (1, 2, 3) if n != leader]
            assert propose(follower, b'SET forbidden 1')[2] == 0
            assert read(follower, 'forbidden') == (0, 0, b'')
            print('PASS: diagnostic proposals are leader-only; uncommitted keys are invisible')

            assert propose(leader, b'SET color blue') == (term, 1, 1)
            module.wait_for(lambda: all((a := applied(n)) is not None and
                                         a[:3] == (1, 1, 1) and a[3] == 0
                                         for n in (1, 2, 3)), 7)
            assert all(read(n, 'color') == (1, 1, b'blue') for n in (1, 2, 3))
            print('PASS: majority-committed SET is applied exactly once on all three nodes')

            # End-to-end binary safety: NUL and 0xff belong to a value, not to framing.
            assert propose(leader, b'SET binary A\x00B\xff') == (term, 2, 1)
            module.wait_for(lambda: all((a := applied(n)) is not None and
                                         a[:3] == (2, 2, 2) for n in (1, 2, 3)), 7)
            assert all(read(n, 'binary') == (1, 2, b'A\x00B\xff') for n in (1, 2, 3))
            print('PASS: binary SET values survive network replication and ordered application')

            stop(follower)
            stop(other)
            assert propose(leader, b'SET hidden still-uncommitted') == (term, 3, 1)
            time.sleep(.5)
            assert applied(leader)[:2] == (2, 2)
            assert read(leader, 'hidden') == (0, 2, b'')
            print('PASS: a leader without quorum cannot expose its uncommitted SET')

            start(follower)
            module.wait_for(lambda: (a := applied(follower)) is not None and
                                        a[:2] == (3, 3), 12)
            module.wait_for(lambda: (a := applied(leader)) is not None and
                                        a[:2] == (3, 3), 7)
            assert read(leader, 'hidden') == (1, 3, b'still-uncommitted')
            assert read(follower, 'hidden') == (1, 3, b'still-uncommitted')
            print('PASS: restored majority commits and applies previously invisible entry')

            assert propose(leader, b'DEL color') == (term, 4, 1)
            module.wait_for(lambda: all((a := applied(n)) is not None and
                                        a[:2] == (4, 4) for n in (leader, follower)), 7)
            assert read(leader, 'color') == (0, 4, b'')
            assert read(follower, 'color') == (0, 4, b'')
            print('PASS: committed DEL removes key on all available replicas')

            assert propose(leader, b'NOT A KNOWN COMMAND') == (term, 5, 1)
            module.wait_for(lambda: all((a := applied(n)) is not None and
                                         a[:2] == (5, 5) and a[3] == 1
                                         for n in (leader, follower)), 7)
            print('PASS: malformed committed command is a deterministic no-op that advances lastApplied')

            start(other)  # loses all memory; must replicate prefix, then apply in order
            module.wait_for(lambda: (a := applied(other)) is not None and
                                        a == (5, 5, 2, 1), 12)
            assert read(other, 'color') == (0, 5, b'')
            assert read(other, 'binary') == (1, 5, b'A\x00B\xff')
            assert read(other, 'hidden') == (1, 5, b'still-uncommitted')
            assert all(applied(n) == (5, 5, 2, 1) for n in (1, 2, 3))
            print('PASS: restarted follower catches up and reconstructs identical committed key-value state')

            # CLI smoke for the added diagnostics.
            for cli, marker in [(('--apply-status', '2'), 'last_applied='),
                                (('--get-demo', '2', 'binary'), 'value_hex=410042ff')]:
                result = subprocess.run([binary, cli[0], str(configs[0]), *cli[1:]],
                                        capture_output=True, text=True, timeout=3)
                assert result.returncode == 0 and marker in result.stdout, result
            print('PASS: --apply-status and --get-demo CLI diagnostics decode framed replies')
        except Exception:
            for f in logs:
                f.flush()
                print(f'--- {f.name} ---', file=sys.stderr)
                print(Path(f.name).read_text()[-5500:], file=sys.stderr)
            raise
        finally:
            for n in (1, 2, 3):
                stop(n)
            for f in logs:
                f.close()


if __name__ == '__main__':
    main(sys.argv[1])
