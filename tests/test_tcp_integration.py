#!/usr/bin/env python3
"""Phase 4 integration tests using only Python's standard library.

Uses temporary configs and ephemeral free ports, so normal repo ports 7001-7003
need not be available. Exercises real TCP sockets and process cleanup.
"""

from __future__ import annotations

import select
import signal
import socket
import subprocess
import sys
import tempfile
from pathlib import Path


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)
    print(f"PASS: {message}")


def unused_ports(count: int) -> list[int]:
    sockets = []
    try:
        for _ in range(count):
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.bind(("127.0.0.1", 0))
            sockets.append(s)
        return [s.getsockname()[1] for s in sockets]
    finally:
        for s in sockets:
            s.close()


def write_configs(directory: Path, ports: list[int]) -> list[Path]:
    result = []
    for index in range(1, 4):
        path = directory / f"node{index}.conf"
        data = [f"node_id={index}"]
        data += [f"peer={i},127.0.0.1,{port}"
                 for i, port in enumerate(ports, 1)]
        path.write_text("\n".join(data) + "\n", encoding="ascii")
        result.append(path)
    return result


def spawn(binary: str, *args: str) -> subprocess.Popen[str]:
    return subprocess.Popen(
        [binary, *args], text=True, encoding="utf-8",
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )


def ready_line(proc: subprocess.Popen[str]) -> str:
    assert proc.stdout is not None
    readable, _, _ = select.select([proc.stdout], [], [], 3.0)
    if not readable:
        raise AssertionError("server did not print readiness line in 3s")
    line = proc.stdout.readline().strip()
    check(line.startswith("Listening: node="), f"server ready ({line})")
    return line


def stop(proc: subprocess.Popen[str]) -> tuple[str, str]:
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
    try:
        return proc.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.communicate(timeout=5)
        raise AssertionError("server failed to stop within 5s")


def main(binary: str) -> None:
    binary = str(Path(binary).resolve())
    with tempfile.TemporaryDirectory(prefix="raft-phase04-") as temp:
        config = write_configs(Path(temp), unused_ports(3))
        active: list[subprocess.Popen[str]] = []
        try:
            node1 = spawn(binary, "--serve", str(config[0]),
                          "--max-connections", "2")
            active.append(node1)
            ready_line(node1)

            # Peer 2 initiates a TCP connection to node 1.
            result = subprocess.run(
                [binary, "--probe", str(config[1]), "1"],
                text=True, capture_output=True, timeout=5, check=False,
            )
            check(result.returncode == 0 and
                  "Connected: local_node=2 peer=1" in result.stdout,
                  "peer-to-peer probe connects to listener")

            # A raw Python client also connects, and sees EOF (no protocol yet).
            port1 = int(config[0].read_text().split("\n")[1].rsplit(",", 1)[-1])
            with socket.create_connection(("127.0.0.1", port1), timeout=2) as sock:
                sock.settimeout(2)
                check(sock.recv(1) == b"", "accepted socket closed without protocol bytes")

            output, errors = node1.communicate(timeout=5)
            check(node1.returncode == 0 and
                  output.count("Accepted:") == 2 and
                  "Stopped: node=1 accepted=2" in output,
                  f"bounded server exits after exactly two connections ({errors.strip()})")

            # Unreachable peer should fail promptly and cleanly.
            failed = subprocess.run(
                [binary, "--probe", str(config[0]), "3"],
                text=True, capture_output=True, timeout=5, check=False,
            )
            check(failed.returncode != 0 and "TCP probe failed:" in failed.stderr,
                  "probe reports refused connection instead of hanging")

            # Two processes cannot simultaneously bind the same IP:port.
            node2 = spawn(binary, "--serve", str(config[1]))
            active.append(node2)
            ready_line(node2)
            duplicate = subprocess.run(
                [binary, "--serve", str(config[1]), "--max-connections", "1"],
                text=True, capture_output=True, timeout=5, check=False,
            )
            check(duplicate.returncode != 0 and "bind " in duplicate.stderr,
                  "occupied listener endpoint rejects second bind")

            probe2 = subprocess.run(
                [binary, "--probe", str(config[2]), "2"],
                text=True, capture_output=True, timeout=5, check=False,
            )
            check(probe2.returncode == 0, "third node config can contact node 2")
            out, _ = stop(node2)
            check(node2.returncode == 0 and "Stopped: node=2" in out,
                  "SIGTERM stops server cleanly")

            for bad_id in ["0", "9999", "-1", "4294967296", "notanumber"]:
                failure = subprocess.run(
                    [binary, "--probe", str(config[2]), bad_id],
                    text=True, capture_output=True, timeout=5, check=False,
                )
                check(failure.returncode != 0, f"invalid peer ID '{bad_id}' rejected")

            bad_count = subprocess.run(
                [binary, "--serve", str(config[0]), "--max-connections", "0"],
                text=True, capture_output=True, timeout=5, check=False,
            )
            check(bad_count.returncode != 0 and
                  "Invalid --max-connections" in bad_count.stderr,
                  "invalid connection limit rejected")
        finally:
            for proc in active:
                if proc.poll() is None:
                    stop(proc)

    print("All Phase 4 TCP integration tests passed.")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_tcp_integration.py PATH_TO_BINARY")
    main(sys.argv[1])
