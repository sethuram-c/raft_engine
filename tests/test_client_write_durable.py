#!/usr/bin/env python3
"""Phase 22: snapshot-WAL committed client-write result survives SIGKILL."""
from __future__ import annotations
import importlib.util
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

spec = importlib.util.spec_from_file_location('support',
    Path(__file__).with_name('test_progress_integration.py'))
assert spec and spec.loader
support = importlib.util.module_from_spec(spec)
spec.loader.exec_module(support)


def main(binary):
    binary = str(Path(binary).resolve())
    with tempfile.TemporaryDirectory(prefix='raft-phase22-durable-') as tmp:
        base = Path(tmp)
        data = base / 'data'
        data.mkdir()
        ports = support.ports_three()
        cfgs = []
        for n in (1, 2, 3):
            cfg = base / f'{n}.conf'
            cfg.write_text(f'node_id={n}\n' + ''.join(
                f'peer={i},127.0.0.1,{p}\n' for i, p in enumerate(ports, 1)))
            cfgs.append(cfg)
        procs = {}
        streams = []

        def start(n):
            f = open(base / f'run-{n}-{len(streams)}.log', 'w+')
            streams.append(f)
            procs[n] = subprocess.Popen([binary, '--elect-snapshot-wal',
                str(cfgs[n - 1]), str(data)], stdout=f, stderr=subprocess.STDOUT)

        def stop(n, kill=False):
            p = procs.get(n)
            if p and p.poll() is None:
                p.send_signal(signal.SIGKILL if kill else signal.SIGTERM)
                try: p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    p.kill(); p.wait(timeout=2)

        def status(n):
            return support.try_rpc(ports[n - 1], 5, support.STATUS)

        def elected():
            states = [status(n) for n in (1, 2)]
            if any(s is None for s in states): return None
            leaders = [n for n, s in enumerate(states, 1) if s[2] == 2]
            if len(leaders) != 1 or states[0][1] != states[1][1]: return None
            return leaders[0]

        try:
            start(1)
            time.sleep(.22)
            start(2)
            leader = support.wait_for(elected, 17)
            command = 'SET durable retained'
            result = subprocess.run([binary, '--write', str(cfgs[2]),
                str(leader), '3000', command], capture_output=True, text=True, timeout=8)
            assert result.returncode == 0 and 'COMMITTED' in result.stdout, result
            assert data.joinpath(f'node-{leader}.snapwal').is_file()
            print('PASS: snapshot-WAL mode acknowledges a client write after quorum commitment')
            stop(1, kill=True)
            stop(2, kill=True)
            start(1)
            time.sleep(.22)
            start(2)
            support.wait_for(lambda: all((s := support.try_rpc(ports[n - 1],
                9, support.LOG)) is not None and s[0] >= 1 for n in (1, 2)), 16)
            old_leader = support.try_rpc(ports[leader - 1], 9, support.LOG)
            assert old_leader is not None and old_leader[2] >= 1, old_leader
            # The other follower may not have observed leaderCommit before
            # SIGKILL. A NEW leader cannot directly commit an older-term
            # entry until a current-term entry becomes majority-replicated.
            print('PASS: acknowledged write survives SIGKILL in committed leader WAL; both logs recover')
        except Exception:
            for f in streams:
                f.flush()
                print(f'--- {f.name} ---', file=sys.stderr)
                print(Path(f.name).read_text()[-5000:], file=sys.stderr)
            raise
        finally:
            for n in tuple(procs): stop(n)
            for f in streams: f.close()


if __name__ == '__main__': main(sys.argv[1])
