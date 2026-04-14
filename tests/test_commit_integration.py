#!/usr/bin/env python3
"""Phase 14: live majority commits, no-quorum rejection and follower notification."""
from __future__ import annotations
import importlib.util
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# Reuse the already-tested Phase 13 wire helpers and ephemeral-port setup.
spec = importlib.util.spec_from_file_location('phase13_progress',
                                              Path(__file__).with_name('test_progress_integration.py'))
assert spec is not None and spec.loader is not None
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def main(binary: str) -> None:
    binary = str(Path(binary).resolve())
    with tempfile.TemporaryDirectory(prefix='raft-phase14-') as directory:
        base = Path(directory)
        ports = module.ports_three()
        configs = []
        for n in (1, 2, 3):
            cfg = base / f'node{n}.conf'
            cfg.write_text(f'node_id={n}\n' + ''.join(
                f'peer={i},127.0.0.1,{port}\n' for i, port in enumerate(ports, 1)))
            configs.append(cfg)
        procs = {}
        files = []

        def start(n: int) -> None:
            f = open(base / f'node{n}-{len(files)}.log', 'w+')
            files.append(f)
            procs[n] = subprocess.Popen([binary, '--elect', str(configs[n-1])],
                                        stdout=f, stderr=subprocess.STDOUT)

        def stop(n: int) -> None:
            p = procs.get(n)
            if p is None or p.poll() is not None:
                return
            p.send_signal(signal.SIGTERM)
            try:
                p.wait(timeout=4)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait(timeout=2)

        def status(n: int):
            return module.try_rpc(ports[n-1], 5, module.STATUS)

        def log(n: int):
            return module.try_rpc(ports[n-1], 9, module.LOG)

        def proposal(n: int, command: bytes):
            typ, payload = module.rpc(ports[n-1], 11, command)
            assert typ == 12 and len(payload) == module.PROPOSE_REPLY.size
            return module.PROPOSE_REPLY.unpack(payload)

        def stable():
            current = {n: status(n) for n in (1, 2, 3)}
            if any(s is None for s in current.values()):
                return None
            leaders = [n for n, s in current.items() if s[2] == 2]
            terms = {s[1] for s in current.values()}
            return (leaders[0], terms.pop()) if len(leaders) == len(terms) == 1 else None

        try:
            for n in (1, 2, 3):
                start(n)
            leader, term = module.wait_for(stable, duration=10)
            followers = [n for n in (1, 2, 3) if n != leader]
            first, second = followers
            assert proposal(leader, b'SET quorum 1') == (term, 1, 1)
            module.wait_for(lambda: all((x := log(n)) is not None and
                                        x[0] == 1 and x[2] == 1 for n in (1, 2, 3)), 7)
            print('PASS: leader commits index 1 after majority; both followers learn leaderCommit')

            stop(first)
            assert proposal(leader, b'SET quorum 2') == (term, 2, 1)
            module.wait_for(lambda: all((x := log(n)) is not None and
                                        x[0] == 2 and x[2] == 2 for n in (leader, second)), 7)
            print('PASS: two of three voters can commit even with one follower offline')

            stop(second)
            assert proposal(leader, b'SET noquorum 3') == (term, 3, 1)
            time.sleep(.7)
            status_only = log(leader)
            assert status_only is not None and status_only[0] == 3 and status_only[2] == 2, status_only
            print('PASS: leader alone cannot commit a new entry without a voting majority')

            start(first)
            module.wait_for(lambda: (x := log(first)) is not None and
                                        x[0] == 3 and x[2] == 3, 10)
            module.wait_for(lambda: (x := log(leader)) is not None and x[2] == 3, 7)
            print('PASS: rejoining follower catches up and restores quorum; commit reaches index 3')

            start(second)
            module.wait_for(lambda: (x := log(second)) is not None and
                                        x[0] == 3 and x[2] == 3, 10)
            print('PASS: previously disconnected follower receives committed prefix on catch-up')

        except Exception:
            for f in files:
                f.flush()
                print(f'--- {f.name} ---', file=sys.stderr)
                print(Path(f.name).read_text()[-6000:], file=sys.stderr)
            raise
        finally:
            for n in (1, 2, 3):
                stop(n)
            for f in files:
                f.close()


if __name__ == '__main__':
    main(sys.argv[1])
