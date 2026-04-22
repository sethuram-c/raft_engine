#!/usr/bin/env python3
"""Phase 22: client COMMITTED vs NOT_LEADER/TIMEOUT and post-timeout commit."""
from __future__ import annotations
import importlib.util
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

spec = importlib.util.spec_from_file_location('progress_support',
                                              Path(__file__).with_name('test_progress_integration.py'))
assert spec and spec.loader
support = importlib.util.module_from_spec(spec)
spec.loader.exec_module(support)
REPLY = struct.Struct('!QQB')
APPLY = struct.Struct('!QQIQ')
READ = struct.Struct('!BQH')


def long_rpc(port: int, kind: int, body: bytes):
    with socket.create_connection(('127.0.0.1', port), timeout=1) as s:
        s.settimeout(4.5)
        s.sendall(support.HEADER.pack(support.MAGIC, 1, kind, 0, len(body)) + body)
        magic, version, typ, flags, length = support.HEADER.unpack(support.read_exact(s, 12))
        assert (magic, version, flags) == (support.MAGIC, 1, 0)
        return typ, support.read_exact(s, length)


def main(binary: str) -> None:
    binary = str(Path(binary).resolve())
    with tempfile.TemporaryDirectory(prefix='raft-phase22-') as directory:
        base = Path(directory)
        ports = support.ports_three()
        configs = []
        for n in (1, 2, 3):
            cfg = base / f'node{n}.conf'
            cfg.write_text(f'node_id={n}\n' + ''.join(
                f'peer={i},127.0.0.1,{port}\n' for i, port in enumerate(ports, 1)))
            configs.append(cfg)
        procs = {}
        streams = []

        def start(n: int):
            f = open(base / f'node{n}-{len(streams)}.log', 'w+')
            streams.append(f)
            procs[n] = subprocess.Popen([binary, '--elect', str(configs[n-1])],
                                        stdout=f, stderr=subprocess.STDOUT)

        def stop(n: int):
            p = procs.get(n)
            if p and p.poll() is None:
                p.send_signal(signal.SIGTERM)
                try:
                    p.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    p.kill()
                    p.wait(timeout=2)

        def status(n):
            return support.try_rpc(ports[n-1], 5, support.STATUS)

        def log(n):
            return support.try_rpc(ports[n-1], 9, support.LOG)

        def leader():
            states = {n: status(n) for n in (1, 2, 3)}
            if any(s is None for s in states.values()):
                return None
            leaders = [n for n, s in states.items() if s[2] == 2]
            terms = {s[1] for s in states.values()}
            return (leaders[0], terms.pop()) if len(leaders) == len(terms) == 1 else None

        def write_raw(n: int, timeout: int, command: bytes):
            typ, body = long_rpc(ports[n-1], 21, struct.pack('!I', timeout) + command)
            assert typ == 22 and len(body) == REPLY.size, (typ, body)
            return REPLY.unpack(body)

        def write_cli(n: int, timeout: int, command: str):
            return subprocess.run([binary, '--write', str(configs[(n % 3)]),
                                   str(n), str(timeout), command],
                                  capture_output=True, text=True, timeout=8)

        try:
            for n in (1, 2, 3):
                start(n)
            elected, term = support.wait_for(leader, duration=12)
            follower, other = [n for n in (1, 2, 3) if n != elected]

            rejected = write_cli(follower, 700, 'SET should_not_write value')
            assert rejected.returncode == 2 and 'NOT_LEADER' in rejected.stdout, rejected
            assert log(follower)[0] == 0
            print('PASS: follower returns NOT_LEADER without appending a client write')

            before = log(elected)[0]
            assert write_raw(elected, 700, b'PUT invalid X')[2] == 3
            assert write_raw(elected, 5, b'SET invalid 1')[2] == 3
            assert log(elected)[0] == before
            print('PASS: invalid command and timeout are rejected without log admission')

            good = write_cli(elected, 2400, 'SET committed yes')
            assert good.returncode == 0 and 'COMMITTED' in good.stdout, good
            entry = log(elected)
            assert entry is not None and entry[0] == 1 and entry[2] == 1
            print('PASS: leader returns COMMITTED only after majority-backed commitIndex')

            raw = write_raw(elected, 2500, b'SET binary A\x00B\xff')
            assert raw[2] == 0 and raw[1] == 2
            support.wait_for(lambda: all((x := log(n)) and x[2] >= 2
                                         for n in (1, 2, 3)), 8)
            print('PASS: binary write commits across three nodes using framed TCP')

            stop(follower)
            stop(other)
            uncertain = write_cli(elected, 450, 'SET later visible')
            assert uncertain.returncode == 3 and 'TIMEOUT' in uncertain.stdout, uncertain
            after = log(elected)
            assert after is not None and after[0] == 3 and after[2] == 2, after
            print('PASS: no quorum yields TIMEOUT and leaves the entry uncommitted')

            start(follower)
            support.wait_for(lambda: (s := log(elected)) is not None and s[2] >= 3, 12)
            support.wait_for(lambda: (s := log(follower)) is not None and s[2] >= 3, 10)
            print('PASS: a timed-out write can later become committed; retry is ambiguous')

        except Exception:
            for stream in streams:
                stream.flush()
                print(f'--- {stream.name} ---', file=sys.stderr)
                print(Path(stream.name).read_text()[-5000:], file=sys.stderr)
            raise
        finally:
            for n in tuple(procs):
                stop(n)
            for stream in streams:
                stream.close()


if __name__ == '__main__':
    main(sys.argv[1])
