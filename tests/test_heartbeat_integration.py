#!/usr/bin/env python3
"""Phase 10: stable elected leader, follower catch-up and leader failover.

Uses real TCP loopback sockets and three independent processes.
No persistence or replicated log entries are claimed here.
"""
from __future__ import annotations

import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

MAGIC = 0x52414654
HEADER = struct.Struct('!IBBHI')
STATUS = struct.Struct('!IQBI')
APPEND = struct.Struct('!QIQQQ')
APPEND_REPLY = struct.Struct('!QB')


def reserve_ports():
    sockets = [socket.socket() for _ in range(3)]
    try:
        for sock in sockets:
            sock.bind(('127.0.0.1', 0))
        return [sock.getsockname()[1] for sock in sockets]
    finally:
        for sock in sockets:
            sock.close()


def read_exact(sock, amount):
    chunks = bytearray()
    while len(chunks) < amount:
        chunk = sock.recv(amount - len(chunks))
        if not chunk:
            raise OSError('truncated RPC')
        chunks.extend(chunk)
    return bytes(chunks)


def request(port, kind, payload=b''):
    with socket.create_connection(('127.0.0.1', port), timeout=.35) as sock:
        sock.settimeout(.5)
        sock.sendall(HEADER.pack(MAGIC, 1, kind, 0, len(payload)) + payload)
        head = HEADER.unpack(read_exact(sock, HEADER.size))
        assert head[:2] == (MAGIC, 1) and head[4] <= 4096
        body = read_exact(sock, head[4])
        return head[2], body


def status(port):
    try:
        kind, body = request(port, 5)
        return STATUS.unpack(body) if kind == 6 and len(body) == STATUS.size else None
    except (OSError, AssertionError, struct.error):
        return None


def wait_for(predicate, seconds=6):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        found = predicate()
        if found:
            return found
        time.sleep(.035)
    raise AssertionError('timed out waiting for Raft condition')


def all_stable(ports, members):
    values = {n: status(ports[n - 1]) for n in members}
    if any(v is None or v[0] != n for n, v in values.items()):
        return None
    terms = {v[1] for v in values.values()}
    leaders = [n for n, v in values.items() if v[2] == 2]
    if len(terms) != 1 or len(leaders) != 1:
        return None
    if any(v[2] != (2 if n == leaders[0] else 0) for n, v in values.items()):
        return None
    return leaders[0], terms.pop(), values


def main(binary):
    binary = str(Path(binary).resolve())
    with tempfile.TemporaryDirectory(prefix='raft-phase10-') as temp:
        root = Path(temp)
        ports = reserve_ports()
        paths = []
        for member in range(1, 4):
            path = root / f'node{member}.conf'
            path.write_text('node_id={}\n'.format(member) + ''.join(
                f'peer={n},127.0.0.1,{port}\n'
                for n, port in enumerate(ports, 1)))
            paths.append(path)
        children = {}
        logfile = []

        def start(n):
            log = open(root / f'{n}.log', 'w+')
            logfile.append(log)
            children[n] = subprocess.Popen(
                [binary, '--elect', str(paths[n - 1])],
                stdout=log, stderr=subprocess.STDOUT)

        def stop(n):
            proc = children[n]
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=2)

        try:
            for n in range(1, 4):
                start(n)
            # During startup one node can briefly win before all peers have
            # begun their RPC/event loops. Demand 0.5s of settled leadership
            # before beginning the strict 1.5-second stability observation.
            def settled():
                candidate = all_stable(ports, (1, 2, 3))
                if not candidate:
                    return None
                leader_id, candidate_term, _ = candidate
                deadline = time.monotonic() + .5
                while time.monotonic() < deadline:
                    cur = all_stable(ports, (1, 2, 3))
                    if not cur or cur[:2] != (leader_id, candidate_term):
                        return None
                    time.sleep(.06)
                return leader_id, candidate_term

            leader, term = wait_for(settled, 8)
            # Reaching a leader is not enough: without heartbeats terms churn.
            end = time.monotonic() + 1.5
            count = 0
            while time.monotonic() < end:
                current = all_stable(ports, (1, 2, 3))
                assert current is not None and current[:2] == (leader, term), \
                    f'heartbeat instability: expected leader={leader} term={term}, got {current}'
                count += 1
                time.sleep(.085)
            assert count >= 4  # time-based stability is primary; CPU load varies
            print('PASS: three nodes maintain one leader and unchanged term for 1.5 seconds')

            # A forged/nonmember ID must be rejected before it can change terms.
            other = next(n for n in (1, 2, 3) if n != leader)
            try:
                request(ports[other - 1], 7, APPEND.pack(term + 100, 999, 0, 0, 0))
            except OSError:
                pass  # rejection deliberately closes the connection
            else:
                raise AssertionError('nonmember AppendEntries unexpectedly accepted')
            assert all_stable(ports, (1, 2, 3))[:2] == (leader, term)
            print('PASS: nonmember heartbeat cannot change follower term')

            stop(leader)
            remaining = tuple(n for n in (1, 2, 3) if n != leader)
            successor, successor_term, _ = wait_for(
                lambda: all_stable(ports, remaining), 7)
            assert successor_term > term and successor != leader
            end = time.monotonic() + 1.0
            while time.monotonic() < end:
                current = all_stable(ports, remaining)
                assert current is not None and current[:2] == (successor, successor_term), \
                    f'failover instability: {current}'
                time.sleep(.08)
            print('PASS: two survivors reelect a stable leader after leader failure')

            start(leader)  # fresh process rejoins with empty in-memory term
            wait_for(lambda: status(ports[leader - 1]) is not None, 3)
            wait_for(lambda: all_stable(ports, (1, 2, 3)), 6)
            print('PASS: restarted third node accepts heartbeat and rejoins as follower')
        except Exception:
            for f in logfile:
                f.flush()
            for f in logfile:
                print(f'--- {f.name} ---', file=sys.stderr)
                print(Path(f.name).read_text()[-4500:], file=sys.stderr)
            raise
        finally:
            for n in list(children):
                stop(n)
            for f in logfile:
                f.close()


if __name__ == '__main__':
    main(sys.argv[1])
