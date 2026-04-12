#!/usr/bin/env python3
"""Phase 12: real TCP AppendEntries with per-process volatile logs."""
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

BIN = str(Path(sys.argv[1]).resolve())

def request(*args, success=True):
    result = subprocess.run([BIN, *map(str, args)], capture_output=True,
                            text=True, timeout=5)
    if success and result.returncode != 0:
        raise AssertionError((args, result.stdout, result.stderr))
    if not success and result.returncode == 0:
        raise AssertionError((args, result.stdout, result.stderr))
    return result.stdout + result.stderr

with tempfile.TemporaryDirectory() as td:
    base = Path(td)
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    port = listener.getsockname()[1]
    listener.close()
    config = base / 'node2.conf'
    template = Path('config/node2.conf').read_text()
    # Avoid assuming a fixed test port; substitute only the node-2 endpoint.
    assert 'peer=2,127.0.0.1,7002' in template
    config.write_text(template.replace('peer=2,127.0.0.1,7002', f'peer=2,127.0.0.1,{port}'))
    client_conf = base / 'node1.conf'
    client_conf.write_text(Path('config/node1.conf').read_text().replace(
        'peer=2,127.0.0.1,7002', f'peer=2,127.0.0.1,{port}'))
    srv = subprocess.Popen([BIN, '--serve', str(config)],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           text=True)
    try:
        for _ in range(80):
            if srv.poll() is not None:
                raise AssertionError('server terminated')
            try:
                with socket.create_connection(('127.0.0.1', port), 0.15):
                    break
            except OSError:
                time.sleep(0.025)
        else:
            raise AssertionError('server never started')
        def log_status():
            return request('--log-status', client_conf, 2)
        assert 'last_index=0' in log_status()
        out = request('--append-demo', client_conf, 2, 3, 0, 0, 'SET a 1')
        assert 'success=1' in out
        assert 'last_index=1 last_term=3' in log_status()
        # Duplicate is idempotent.
        assert 'success=1' in request('--append-demo', client_conf, 2, 3, 0, 0, 'SET a 1')
        assert 'last_index=1' in log_status()
        # Prev index gap should be rejected without modifying the log.
        assert 'success=0' in request('--append-demo', client_conf, 2, 3, 9, 3,
                                       'BAD', success=False)
        assert 'last_index=1' in log_status()
        assert 'success=1' in request('--append-demo', client_conf, 2, 4, 1, 3,
                                       'SET b 2')
        assert 'last_index=2 last_term=4' in log_status()
        # conflicting term at position 2 gets replaced.
        assert 'success=1' in request('--append-demo', client_conf, 2, 5, 1, 3,
                                       'SET b revised')
        assert 'last_index=2 last_term=5' in log_status()
        # Stale term cannot alter log.
        assert 'success=0' in request('--append-demo', client_conf, 2, 4, 1, 3,
                                       'STALE', success=False)
        assert 'last_index=2 last_term=5' in log_status()
        # Malformed message (bad entry count) => connection closed, no log mutation.
        def wire(typ, data):
            return struct.pack('!IBBHI', 0x52414654, 1, typ, 0, len(data)) + data
        malformed = struct.pack('!QIQQQH', 6, 1, 0, 0, 0, 17)
        with socket.create_connection(('127.0.0.1', port), 1) as sock:
            sock.settimeout(1)
            sock.sendall(wire(7, malformed))
            assert sock.recv(1) == b''
        assert 'last_index=2 last_term=5' in log_status()
        # A genuine two-entry binary AppendEntries RPC traverses the socket.
        def recv_exact(sock, count):
            result = bytearray()
            while len(result) < count:
                part = sock.recv(count - len(result))
                assert part, 'truncated response'
                result.extend(part)
            return bytes(result)
        def wire(typ, data):
            return struct.pack('!IBBHI', 0x52414654, 1, typ, 0, len(data)) + data
        cmds = [b'BINARY\x00COMMAND', b'SET c 9']
        multi = struct.pack('!QIQQQH', 6, 1, 2, 5, 3, 2)
        multi += b''.join(struct.pack('!QH', 6, len(cmd)) + cmd for cmd in cmds)
        with socket.create_connection(('127.0.0.1', port), 1) as sock:
            sock.settimeout(1)
            sock.sendall(wire(7, multi))
            response_header = recv_exact(sock, 12)
            magic, version, typ, reserved, length = struct.unpack('!IBBHI', response_header)
            assert (magic, version, typ, reserved, length) == (0x52414654, 1, 8, 0, 9)
            reply = recv_exact(sock, 9)
            assert struct.unpack('!QB', reply) == (6, 1)
        assert 'last_index=4 last_term=6 commit_index=3' in log_status()
        # Committed entries cannot be overwritten with a conflicting suffix.
        conflict = struct.pack('!QIQQQH', 7, 1, 2, 5, 3, 1)
        conflict += struct.pack('!QH', 7, 3) + b'BAD'
        with socket.create_connection(('127.0.0.1', port), 1) as sock:
            sock.settimeout(1)
            sock.sendall(wire(7, conflict))
            response = recv_exact(sock, 21)
            assert struct.unpack('!QB', response[12:]) == (7, 0)
        assert 'last_index=4 last_term=6 commit_index=3' in log_status()
        print('PASS: Phase 12 live append, multi-entry binary batch, commit limit, '
              'conflict repair, stale term, malformed batch')
    finally:
        srv.terminate()
        try:
            srv.communicate(timeout=4)
        except subprocess.TimeoutExpired:
            srv.kill()
            srv.communicate(timeout=3)
