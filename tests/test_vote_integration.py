#!/usr/bin/env python3
"""Phase 8 RequestVote integration test over actual Linux loopback TCP."""
from __future__ import annotations

import concurrent.futures
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HEADER = struct.Struct('!IBBHI')
REQ = struct.Struct('!QIQQ')
RSP = struct.Struct('!QB')
MAGIC = 0x52414654


def check(condition: bool, title: str) -> None:
    if not condition:
        raise AssertionError(title)
    print('PASS:', title)


def free_ports() -> list[int]:
    sockets = []
    try:
        for _ in range(3):
            sock = socket.socket()
            sock.bind(('127.0.0.1', 0))
            sockets.append(sock)
        return [s.getsockname()[1] for s in sockets]
    finally:
        for sock in sockets:
            sock.close()


def request_raw(port: int, req: bytes) -> bytes:
    frame = HEADER.pack(MAGIC, 1, 3, 0, len(req)) + req
    with socket.create_connection(('127.0.0.1', port), timeout=3) as sock:
        sock.settimeout(3)
        sock.sendall(frame)
        sock.shutdown(socket.SHUT_WR)
        data = bytearray()
        while True:
            chunk = sock.recv(100)
            if not chunk:
                break
            data.extend(chunk)
    return bytes(data)


def decode_response(raw: bytes) -> tuple[int, int]:
    check(len(raw) == HEADER.size + RSP.size, 'response has exact wire length')
    check(HEADER.unpack(raw[:HEADER.size]) == (MAGIC, 1, 4, 0, RSP.size),
          'response has RequestVoteResponse frame type')
    return RSP.unpack(raw[HEADER.size:])


def main(binary: str) -> None:
    binary = str(Path(binary).resolve())
    with tempfile.TemporaryDirectory(prefix='raft-phase08-') as directory:
        configs = []
        ports = free_ports()
        for node_id in range(1, 4):
            config = Path(directory) / f'node{node_id}.conf'
            config.write_text(
                f'node_id={node_id}\n' +
                ''.join(f'peer={i},127.0.0.1,{port}\n' for i, port in enumerate(ports, 1)),
                encoding='ascii')
            configs.append(config)
        proc = subprocess.Popen([binary, '--serve', str(configs[0])],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True)
        try:
            assert proc.stdout is not None
            ready, _, _ = select.select([proc.stdout], [], [], 3)
            check(bool(ready), 'server starts')
            check(proc.stdout.readline().startswith('Listening: node=1'),
                  'server prints readiness line')

            def cli(candidate: int, term: int, index: int = 0, last_term: int = 0):
                return subprocess.run([binary, '--request-vote', str(configs[candidate - 1]),
                                       '1', str(term), str(index), str(last_term)],
                                      capture_output=True, text=True, timeout=5)

            a = cli(2, 1)
            check(a.returncode == 0 and 'term=1 granted=1' in a.stdout,
                  'candidate 2 receives first vote in term 1')
            b = cli(3, 1)
            check(b.returncode == 0 and 'term=1 granted=0' in b.stdout,
                  'candidate 3 rejected within same term')
            check('term=1 granted=1' in cli(2, 1).stdout,
                  'original candidate may retry within term')
            c = cli(3, 2)
            check(c.returncode == 0 and 'term=2 granted=1' in c.stdout,
                  'higher-term candidate gets vote')
            stale = cli(2, 1)
            check(stale.returncode == 0 and 'term=2 granted=0' in stale.stdout,
                  'stale request sees current term')
            invalid = cli(2, 0)
            check(invalid.returncode != 0 and 'Invalid RequestVote' in invalid.stderr,
                  'invalid term zero rejected in CLI before networking')
            malformed = request_raw(ports[0], b'123')
            check(malformed == b'', 'malformed RequestVote payload receives no response')
            malformed_id = request_raw(ports[0], REQ.pack(3, 0, 0, 0))
            check(malformed_id == b'', 'candidate ID zero is rejected')
            check(request_raw(ports[0], REQ.pack(3, 99, 0, 0)) == b'',
                  'candidate not in three-node cluster is rejected')
            check(request_raw(ports[0], REQ.pack(3, 1, 0, 0)) == b'',
                  'local node cannot request its own vote via the network')
            term, granted = decode_response(request_raw(ports[0], REQ.pack(3, 2, 0, 0)))
            check((term, granted) == (3, 1), 'raw binary request returns correct vote')

            # Race check: both candidates ask in the same fresh term concurrently.
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                results = list(pool.map(lambda candidate: cli(candidate, 4), [2, 3]))
            check(all(x.returncode == 0 for x in results),
                  'concurrent requests get valid replies')
            grants = sum('term=4 granted=1' in x.stdout for x in results)
            check(grants == 1, 'at most one competing candidate gets vote in term 4')
            check(proc.poll() is None, 'server remains alive after vote traffic')
        finally:
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
            try:
                proc.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.communicate(timeout=5)
        check(proc.returncode == 0, 'server shuts down without worker leaks')
        print('All Phase 8 RequestVote integration checks passed.')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('Usage: test_vote_integration.py PATH_TO_BINARY')
    main(sys.argv[1])
