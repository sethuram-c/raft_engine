#!/usr/bin/env python3
"""Phase 17: append-only WAL, SIGKILL recovery, torn tail, CRC and isolation."""
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
    'support', Path(__file__).with_name('test_progress_integration.py'))
assert spec and spec.loader
support = importlib.util.module_from_spec(spec)
spec.loader.exec_module(support)
APPLY = struct.Struct('!QQIQ')
READ = struct.Struct('!BQH')


def main(binary: str) -> None:
    binary = str(Path(binary).resolve())
    with tempfile.TemporaryDirectory(prefix='raft-phase17-') as directory:
        root = Path(directory)
        data = root / 'durable-data'
        ports = support.ports_three()
        cfgs = []
        for node in (1, 2, 3):
            cfg = root / f'node{node}.conf'
            cfg.write_text(f'node_id={node}\n' + ''.join(
                f'peer={i},127.0.0.1,{port}\n'
                for i, port in enumerate(ports, 1)))
            cfgs.append(cfg)
        procs = {}
        streams = []

        def start(node: int) -> None:
            logfile = open(root / f'node{node}-{len(streams)}.log', 'w+')
            streams.append(logfile)
            procs[node] = subprocess.Popen([
                binary, '--elect-wal', str(cfgs[node-1]), str(data)],
                stdout=logfile, stderr=subprocess.STDOUT)

        def stop(node: int, crash: bool = False) -> None:
            p = procs.get(node)
            if p is None or p.poll() is not None:
                return
            p.send_signal(signal.SIGKILL if crash else signal.SIGTERM)
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait(timeout=3)

        def status(node):
            return support.try_rpc(ports[node-1], 5, support.STATUS)

        def log(node):
            return support.try_rpc(ports[node-1], 9, support.LOG)

        def applied(node):
            return support.try_rpc(ports[node-1], 13, APPLY)

        def get(node: int, key: str):
            typ, payload = support.rpc(ports[node-1], 15, key.encode())
            assert typ == 16
            found, index, length = READ.unpack(payload[:READ.size])
            assert len(payload) == READ.size + length
            return found, index, payload[READ.size:]

        def propose(node: int, command: bytes):
            typ, payload = support.rpc(ports[node-1], 11, command)
            assert typ == 12
            return support.PROPOSE_REPLY.unpack(payload)

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
            leader, term = support.wait_for(stable, duration=12)
            assert propose(leader, b'SET durable A\x00B') == (term, 1, 1)
            support.wait_for(lambda: all((a := applied(n)) is not None and a[:2] == (1, 1)
                                         for n in (1, 2, 3)), duration=10)
            assert all(get(n, 'durable') == (1, 1, b'A\x00B') for n in (1, 2, 3))
            for n in (1, 2, 3):
                journal = data / f'node-{n}.wal'
                assert journal.stat().st_size > 50
                assert journal.read_bytes().startswith(b'RAFT17WL')
            print('PASS: WAL nodes persist committed binary entry in per-node journals')

            # Abruptly terminate all three servers: no graceful state flush.
            for n in (1, 2, 3):
                stop(n, crash=True)
            for n in (1, 2, 3):
                start(n)
            leader2, term2 = support.wait_for(stable, duration=15)
            assert term2 > 0 and leader2 in (1, 2, 3)
            support.wait_for(lambda: all((a := applied(n)) is not None and a[:2] == (1, 1)
                                         for n in (1, 2, 3)), duration=10)
            assert all(get(n, 'durable') == (1, 1, b'A\x00B') for n in (1, 2, 3))
            print('PASS: SIGKILL and restart recover committed log, metadata and KV replay')

            # An isolated leader must save a new entry, but must NOT apply it.
            follower, other = [n for n in (1, 2, 3) if n != leader2]
            # Kill both followers without sequential graceful-shutdown delays.
            # A long delay could allow an independent election to depose the
            # selected leader before the no-quorum proposal is sent.
            for n in (follower, other):
                procs[n].send_signal(signal.SIGKILL)
            for n in (follower, other):
                procs[n].wait(timeout=5)
            term_before = status(leader2)[1]
            assert propose(leader2, b'SET pending not-committed') == (term_before, 2, 1)
            assert log(leader2)[:3] == (2, term_before, 1)
            assert get(leader2, 'pending')[0] == 0
            stop(leader2, crash=True)
            start(leader2)
            support.wait_for(lambda: (v := log(leader2)) is not None and
                                        v[0] == 2 and v[2] == 1, duration=4)
            assert get(leader2, 'pending')[0] == 0
            print('PASS: uncommitted local suffix survives crash but is not applied')
            stop(leader2, crash=True)

            # A simulated torn final append must be truncated durably on replay.
            journal = data / f'node-{leader2}.wal'
            original_size = journal.stat().st_size
            with journal.open('ab') as f:
                f.write(b'\x00\x00\x00\x24\x00\x00\x00\x00\x01\x02')
            assert journal.stat().st_size == original_size + 10
            start(leader2)
            support.wait_for(lambda: (v := log(leader2)) is not None and
                                        v[0] == 2 and v[2] == 1, duration=5)
            assert journal.stat().st_size == original_size
            assert get(leader2, 'pending')[0] == 0
            print('PASS: incomplete WAL tail is truncated and committed state remains intact')
            stop(leader2, crash=True)

            # Corruption must fail closed; never silently reinitialize from scratch.
            file = data / f'node-{leader2}.wal'
            bytes_before = file.read_bytes()
            broken = bytearray(bytes_before)
            broken[29] ^= 0x11
            file.write_bytes(broken)
            result = subprocess.run([
                binary, '--elect-wal', str(cfgs[leader2 - 1]), str(data)],
                capture_output=True, text=True, timeout=6)
            assert result.returncode != 0, result
            assert 'corrupt' in result.stderr.lower(), result.stderr
            assert file.read_bytes() == bytes(broken)
            print('PASS: corrupted complete WAL record blocks startup without overwriting state')
        except Exception:
            for s in streams:
                s.flush()
                print(f'--- {s.name} ---', file=sys.stderr)
                print(Path(s.name).read_text()[-7000:], file=sys.stderr)
            raise
        finally:
            for n in (1, 2, 3):
                stop(n)
            for stream in streams:
                stream.close()


if __name__ == '__main__':
    main(sys.argv[1])
