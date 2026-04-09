#!/usr/bin/env python3
"""Phase 9: real 3-process elections, zero-quorum and two-node quorum.

No heartbeat implemented. Only assert a momentary leader, not stable lease.
"""
from __future__ import annotations

import socket
import struct
import subprocess
import tempfile
import time
import signal
import sys
from pathlib import Path

HEADER = struct.Struct('!IBBHI')
STATUS = struct.Struct('!IQBI')
MAGIC = 0x52414654


def free_ports():
    sockets = [socket.socket() for _ in range(3)]
    try:
        for s in sockets:
            s.bind(('127.0.0.1', 0))
        return [s.getsockname()[1] for s in sockets]
    finally:
        for s in sockets: s.close()


def status(port):
    try:
        with socket.create_connection(('127.0.0.1', port), timeout=0.18) as s:
            s.settimeout(0.2)
            s.sendall(HEADER.pack(MAGIC, 1, 5, 0, 0))
            raw = bytearray()
            while len(raw) < HEADER.size + STATUS.size:
                chunk = s.recv(HEADER.size + STATUS.size - len(raw))
                if not chunk: return None
                raw.extend(chunk)
        if HEADER.unpack(raw[:HEADER.size]) != (MAGIC, 1, 6, 0, STATUS.size):
            return None
        return STATUS.unpack(raw[HEADER.size:])
    except (OSError, ValueError, struct.error):
        return None


def check(condition, message):
    assert condition, message
    print('PASS:', message)


def main(binary):
    binary = str(Path(binary).resolve())
    with tempfile.TemporaryDirectory(prefix='raft-phase09-') as temp:
        directory = Path(temp)
        ports = free_ports()
        configs = []
        for node_id in range(1, 4):
            path = directory / f'node{node_id}.conf'
            path.write_text(f'node_id={node_id}\n' + ''.join(
                f'peer={i},127.0.0.1,{port}\n' for i, port in enumerate(ports, 1)))
            configs.append(path)
        processes = {}
        logs = []
        def start(node_id):
            log = open(directory / f'{node_id}.log', 'w+')
            logs.append(log)
            processes[node_id] = subprocess.Popen(
                [binary, '--elect', str(configs[node_id - 1])],
                stdout=log, stderr=subprocess.STDOUT)
        try:
            start(1)
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                value = status(ports[0])
                if value and value[1] >= 1 and value[2] == 1:
                    break
                time.sleep(0.03)
            else:
                raise AssertionError('one isolated voter should become candidate')
            check(value[2] == 1 and value[1] >= 1, 'isolated node becomes candidate')
            time.sleep(0.25)
            check(status(ports[0])[2] != 2, 'single node cannot elect itself in a three-voter cluster')
            start(2)
            deadline = time.monotonic() + 4
            found = None
            while time.monotonic() < deadline:
                current = [status(p) for p in ports[:2]]
                leaders = [item for item in current if item and item[2] == 2]
                if leaders:
                    found = leaders[0]
                    break
                time.sleep(0.025)
            check(found is not None, 'two of three nodes elect a majority-backed leader')
            check(found[1] >= 1 and found[3] != 0, 'elected leader has a term and self-vote')
            start(3)
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                if status(ports[2]) is not None: break
                time.sleep(0.03)
            check(status(ports[2]) is not None, 'third node joins and responds to status')
            # Exercise CLI and avoid promising stability without heartbeats.
            cli = subprocess.run([binary, '--status', str(configs[0]), '2'],
                                  capture_output=True, text=True, timeout=3)
            check(cli.returncode == 0 and 'Status: node=2 role=' in cli.stdout,
                  'status CLI decodes binary response over TCP')
        finally:
            for proc in processes.values():
                if proc.poll() is None: proc.send_signal(signal.SIGTERM)
            for proc in processes.values():
                try: proc.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    proc.kill(); proc.wait(timeout=2)
            for log in logs: log.close()


if __name__ == '__main__':
    main(sys.argv[1])
