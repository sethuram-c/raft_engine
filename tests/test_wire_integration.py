#!/usr/bin/env python3
"""End-to-end Phase 5 wire framing tests against real localhost sockets.

No extra Python dependencies. Starts one process using a temporary Raft
configuration with an ephemeral port; checks the exact on-wire bytes.
"""
from __future__ import annotations

import select
import socket
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HEADER = struct.Struct("!IBBHI")
MAGIC = 0x52414654
MAX_PAYLOAD = 4096


def check(expr: bool, desc: str) -> None:
    if not expr:
        raise AssertionError(desc)
    print("PASS:", desc)


def make_frame(body: bytes, message_type: int = 1) -> bytes:
    return HEADER.pack(MAGIC, 1, message_type, 0, len(body)) + body


def read_exact(sock: socket.socket, count: int) -> bytes:
    data = bytearray()
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise AssertionError(f"unexpected EOF after {len(data)}/{count} bytes")
        data += chunk
    return bytes(data)


def raw_request(port: int, parts: list[bytes]) -> bytes:
    with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
        sock.settimeout(2)
        for piece in parts:
            sock.sendall(piece)
        sock.shutdown(socket.SHUT_WR)
        response = bytearray()
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            response += chunk
        return bytes(response)


def main(binary: str) -> None:
    binary = str(Path(binary).resolve())
    with tempfile.TemporaryDirectory(prefix="raft-phase05-") as temp:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as fd:
            fd.bind(("127.0.0.1", 0))
            port = fd.getsockname()[1]
        peer_ports = [port, 1 if port != 1 else 3, 2 if port != 2 else 4]
        files = []
        for node in range(1, 4):
            path = Path(temp) / f"node{node}.conf"
            path.write_text("node_id=" + str(node) + "\n" +
                            "".join(f"peer={i},127.0.0.1,{peer_ports[i - 1]}\n"
                                    for i in range(1, 4)), encoding="ascii")
            files.append(path)
        # Avoid collisions for peer2/peer3 config validation by generating distinct
        # in-range ports. Only node1's port needs to be free at bind time.
        if port > 65533:
            raise AssertionError("unexpected upper-bound ephemeral port")
        # 13 connections: CLI ping, binary, zero, fragmented, max-length, bad magic,
        # version, type, flags, length, truncated payload, truncated header.
        cases = 13
        proc = subprocess.Popen(
            [binary, "--serve", str(files[0]), "--max-connections", str(cases)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        try:
            assert proc.stdout is not None
            ready, _, _ = select.select([proc.stdout], [], [], 3)
            check(bool(ready), "server started")
            line = proc.stdout.readline()
            check(line.startswith("Listening: node=1"), "server announces readiness")

            result = subprocess.run(
                [binary, "--ping", str(files[1]), "1", "hello-raft"],
                capture_output=True, text=True, timeout=5,
            )
            check(result.returncode == 0 and
                  "PONG: local_node=2 peer=1 bytes=10 message=hello-raft"
                  in result.stdout, "CLI node-to-node PING/PONG")

            payload = b"\x00\xff\x12A\x00"
            check(raw_request(port, [make_frame(payload)]) ==
                  make_frame(payload, 2), "binary payload echoed byte-for-byte")
            check(raw_request(port, [make_frame(b"")]) ==
                  make_frame(b"", 2), "zero-length payload accepted")
            large = bytes(i % 256 for i in range(MAX_PAYLOAD))
            check(raw_request(port, [make_frame(large)]) == make_frame(large, 2),
                  "maximum 4096-byte payload echoed")
            frame = make_frame(b"split-into-chunks")
            check(raw_request(port, [frame[:1], frame[1:5], frame[5:12],
                                     frame[12:15], frame[15:]]) ==
                  make_frame(b"split-into-chunks", 2),
                  "fragmented header and payload reassembled")

            for tag, mutation in [
                ("wrong magic", {"magic": 0}),
                ("wrong version", {"version": 2}),
                ("unknown message type", {"type": 3}),
                ("reserved flags", {"flags": 1}),
                ("oversized declared length", {"length": MAX_PAYLOAD + 1}),
            ]:
                fields = {"magic": MAGIC, "version": 1, "type": 1,
                          "flags": 0, "length": 0}
                fields.update(mutation)
                bad = HEADER.pack(fields["magic"], fields["version"],
                                  fields["type"], fields["flags"], fields["length"])
                check(raw_request(port, [bad]) == b"", f"reject {tag}")
            check(raw_request(port, [HEADER.pack(MAGIC, 1, 1, 0, 5) + b"ab"])
                  == b"", "reject truncated payload")
            check(raw_request(port, [b"RAF"]) == b"", "reject truncated header")
            check(raw_request(port, [make_frame(b"bad", 2)]) == b"",
                  "reject PONG sent as a client request")

            stdout, stderr = proc.communicate(timeout=5)
            check(proc.returncode == 0 and "Stopped: node=1 accepted=13" in stdout,
                  "sequential server exits after specified connection count")
            check("Rejected frame:" in stderr, "protocol failures are logged")
            print("All Phase 5 TCP wire integration tests passed.")
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.communicate(timeout=5)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_wire_integration.py PATH_TO_BINARY")
    main(sys.argv[1])
