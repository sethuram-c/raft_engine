#!/usr/bin/env python3
"""Phase 18: offline and live WAL checkpoints; restart, locking, CRC, post-checkpoint append."""
from __future__ import annotations
import importlib.util
import signal
import struct
import subprocess
import sys
import tempfile
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
    with tempfile.TemporaryDirectory(prefix='raft-phase18-') as directory:
        root = Path(directory)
        data = root / 'wal-data'
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

        def start(node):
            stream = open(root / f'{node}-run-{len(streams)}.log', 'w+')
            streams.append(stream)
            procs[node] = subprocess.Popen(
                [binary, '--elect-wal', str(cfgs[node-1]), str(data)],
                stdout=stream, stderr=subprocess.STDOUT)

        def stop(node):
            proc = procs.get(node)
            if proc and proc.poll() is None:
                proc.send_signal(signal.SIGKILL)
                proc.wait(timeout=5)

        def stable():
            statuses = {n: support.try_rpc(ports[n-1], 5, support.STATUS)
                        for n in (1, 2, 3)}
            if any(s is None for s in statuses.values()):
                return None
            leaders = [n for n, s in statuses.items() if s[2] == 2]
            terms = {s[1] for s in statuses.values()}
            return (leaders[0], terms.pop()) if len(leaders) == len(terms) == 1 else None

        def propose(leader, value):
            typ, payload = support.rpc(ports[leader-1], 11, value)
            assert typ == 12
            return support.PROPOSE_REPLY.unpack(payload)

        def applied(node):
            return support.try_rpc(ports[node-1], 13, APPLY)

        def get(node, key):
            typ, payload = support.rpc(ports[node-1], 15, key.encode())
            assert typ == 16
            found, index, length = READ.unpack(payload[:READ.size])
            assert len(payload) == READ.size + length
            return found, index, payload[READ.size:]

        def compact(node):
            return subprocess.run(
                [binary, '--compact-wal', str(cfgs[node-1]), str(data)],
                capture_output=True, text=True, timeout=8)

        def file(node):
            return data / f'node-{node}.wal'

        try:
            for n in (1, 2, 3):
                start(n)
            leader, term = support.wait_for(stable, duration=16)
            assert propose(leader, b'SET checkpoint A\x00B') == (term, 1, 1)
            support.wait_for(lambda: all((v := applied(n)) is not None and
                                         v[:2] == (1, 1) for n in (1, 2, 3)), duration=10)
            assert all(get(n, 'checkpoint') == (1, 1, b'A\x00B') for n in (1, 2, 3))
            locked = compact(leader)
            assert locked.returncode != 0 and 'lock' in locked.stderr.lower(), locked
            print('PASS: running WAL server holds exclusive lock against offline checkpoint')

            for n in (1, 2, 3):
                stop(n)
            sizes_before = {n: file(n).stat().st_size for n in (1, 2, 3)}
            for n in (1, 2, 3):
                run = compact(n)
                assert run.returncode == 0, run.stderr
                assert f'node={n}' in run.stdout and 'records=1' in run.stdout
                image = file(n).read_bytes()
                assert image.startswith(b'RAFT17WL')
                assert len(image) < sizes_before[n], (n, len(image), sizes_before[n])
                assert struct.unpack('!Q', image[28:36])[0] == 1
            print('PASS: atomic offline checkpoints shrink all three journals to one record')

            # Torn compaction temp is NOT authoritative; the valid WAL wins.
            orphan = data / 'node-1.wal.compact.tmp'
            orphan.write_bytes(b'incomplete checkpoint image')
            for n in (1, 2, 3):
                start(n)
            support.wait_for(lambda: stable(), duration=16)
            assert not orphan.exists()
            support.wait_for(lambda: all((a := applied(n)) is not None and
                                         a[:2] == (1, 1) for n in (1, 2, 3)), duration=10)
            assert all(get(n, 'checkpoint') == (1, 1, b'A\x00B') for n in (1, 2, 3))
            print('PASS: SIGKILL, checkpoint replay and state-machine recovery; orphan temp ignored')

            leader, term = support.wait_for(stable, duration=16)
            accepted = propose(leader, b'SET after yes')
            assert accepted == (term, 2, 1), accepted
            support.wait_for(lambda: all((a := applied(n)) is not None and
                                         a[:2] == (2, 2) for n in (1, 2, 3)), duration=10)
            assert all(get(n, 'after') == (1, 2, b'yes') for n in (1, 2, 3))
            print('PASS: new delta records append and commit normally after checkpoint')

            for n in (1, 2, 3):
                stop(n)
            # Compaction must never silently accept a complete corrupt record.
            damaged = bytearray(file(1).read_bytes())
            damaged[20 + 8 + 8] ^= 0x5A
            file(1).write_bytes(damaged)
            denied = compact(1)
            assert denied.returncode != 0 and 'corrupt' in denied.stderr.lower(), denied
            assert file(1).read_bytes() == damaged
            print('PASS: complete checksum corruption blocks checkpoint without file replacement')
        except Exception:
            for stream in streams:
                stream.flush()
                print(f'--- {stream.name} ---', file=sys.stderr)
                print(Path(stream.name).read_text()[-4000:], file=sys.stderr)
            raise
        finally:
            for n in (1, 2, 3):
                stop(n)
            for stream in streams:
                stream.close()


if __name__ == '__main__':
    main(sys.argv[1])
