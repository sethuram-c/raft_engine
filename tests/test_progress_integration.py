#!/usr/bin/env python3
"""Phase 13: live leader-driven AppendEntries, late join and restart catch-up."""
from __future__ import annotations
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HEADER = struct.Struct('!IBBHI')
STATUS = struct.Struct('!IQBI')
LOG = struct.Struct('!QQQ')
PROPOSE_REPLY = struct.Struct('!QQB')
MAGIC = 0x52414654


def ports_three():
    sockets = [socket.socket() for _ in range(3)]
    try:
        for s in sockets:
            s.bind(('127.0.0.1', 0))
        return [s.getsockname()[1] for s in sockets]
    finally:
        for s in sockets:
            s.close()


def read_exact(s, n):
    b = bytearray()
    while len(b) < n:
        part = s.recv(n - len(b))
        if not part:
            raise OSError('connection closed')
        b += part
    return bytes(b)


def rpc(port, kind, body=b''):
    with socket.create_connection(('127.0.0.1', port), timeout=.4) as s:
        s.settimeout(.6)
        s.sendall(HEADER.pack(MAGIC, 1, kind, 0, len(body)) + body)
        magic, version, k, flags, n = HEADER.unpack(read_exact(s, 12))
        assert (magic, version, flags) == (MAGIC, 1, 0) and n <= 4096
        return k, read_exact(s, n)


def try_rpc(port, kind, outstruct):
    try:
        k, b = rpc(port, kind)
        if k != kind + 1 or len(b) != outstruct.size:
            return None
        return outstruct.unpack(b)
    except (OSError, AssertionError, struct.error):
        return None


def wait_for(f, duration=8):
    limit = time.monotonic() + duration
    while time.monotonic() < limit:
        val = f()
        if val:
            return val
        time.sleep(.06)
    raise AssertionError('timed out waiting for cluster condition')


def main(binary):
    binary = str(Path(binary).resolve())
    with tempfile.TemporaryDirectory(prefix='raft-phase13-') as tmp:
        base = Path(tmp)
        ports = ports_three()
        configs = []
        for n in range(1, 4):
            config = base / f'{n}.conf'
            config.write_text(f'node_id={n}\n' + ''.join(
                f'peer={i},127.0.0.1,{port}\n' for i, port in enumerate(ports, 1)))
            configs.append(config)
        processes = {}
        logs = []

        def start(n):
            f = open(base / f'{n}-{len(logs)}.log', 'w+')
            logs.append(f)
            processes[n] = subprocess.Popen([binary, '--elect', str(configs[n-1])],
                                           stdout=f, stderr=subprocess.STDOUT)

        def stop(n):
            p = processes[n]
            if p.poll() is None:
                p.send_signal(signal.SIGTERM)
                try:
                    p.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    p.kill()
                    p.wait(timeout=2)

        def status(n):
            return try_rpc(ports[n-1], 5, STATUS)

        def log(n):
            return try_rpc(ports[n-1], 9, LOG)

        def stable():
            values = {n: status(n) for n in (1, 2, 3)}
            if any(v is None for v in values.values()):
                return None
            terms = {v[1] for v in values.values()}
            leaders = [n for n, v in values.items() if v[2] == 2]
            if len(leaders) != 1 or len(terms) != 1:
                return None
            return leaders[0], terms.pop()

        def proposed(n, command):
            typ, reply = rpc(ports[n-1], 11, command)
            assert typ == 12 and len(reply) == PROPOSE_REPLY.size
            return PROPOSE_REPLY.unpack(reply)

        try:
            for n in range(1, 4):
                start(n)
            leader, term = wait_for(stable)
            follower = next(n for n in (1, 2, 3) if n != leader)
            other = next(n for n in (1, 2, 3) if n not in (leader, follower))
            assert proposed(follower, b'NOT ALLOWED')[2] == 0
            print('PASS: follower rejects leader-only diagnostic proposals')
            cli = subprocess.run([binary, '--propose-demo', str(configs[follower-1]),
                                  str(leader), 'SET alpha 1'],
                                 capture_output=True, text=True, timeout=3)
            assert cli.returncode == 0 and 'accepted=1' in cli.stdout, cli
            assert 'index=1' in cli.stdout and 'uncommitted' in cli.stdout
            wait_for(lambda: all(log(n) and log(n)[0] == 1 for n in (1, 2, 3)))
            print('PASS: elected leader automatically replicates a new entry to both followers')
            stop(follower)
            ans = proposed(leader, b'SET beta 2')
            assert ans == (term, 2, 1), ans
            wait_for(lambda: log(other) and log(other)[0] == 2)
            print('PASS: available follower progresses despite a disconnected peer')
            start(follower)  # loses both entries: follower progress must back off
            wait_for(lambda: log(follower) and log(follower)[0] == 2, 9)
            all_logs = [log(n) for n in (1, 2, 3)]
            assert all(x is not None and x[0] == 2 and x[2] == 0 for x in all_logs), all_logs
            print('PASS: restarted, empty follower catches up automatically using nextIndex retries')
            ans = proposed(leader, b'SET gamma 3')
            assert ans == (term, 3, 1), ans
            wait_for(lambda: all(log(n) and log(n)[0] == 3 for n in (1, 2, 3)))
            assert stable() == (leader, term)
            print('PASS: subsequent replication remains healthy and uncommitted')
        except Exception:
            for f in logs:
                f.flush()
                print(f'--- {f.name} ---', file=sys.stderr)
                print(Path(f.name).read_text()[-4500:], file=sys.stderr)
            raise
        finally:
            for n in list(processes):
                stop(n)
            for f in logs:
                f.close()


if __name__ == '__main__':
    main(sys.argv[1])
