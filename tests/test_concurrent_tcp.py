#!/usr/bin/env python3
"""Phase 6: real concurrent clients, bounded worker saturation and SIGTERM.

Uses temporary loopback ports, no third-party Python modules.
"""
from __future__ import annotations

import concurrent.futures
import os
import select
import signal
import socket
import struct
import subprocess
import sys
import threading
import tempfile
import time
from pathlib import Path

MAGIC = 0x52414654
HEADER = struct.Struct('!IBBHI')
WORKERS = 8


def check(condition: bool, label: str) -> None:
    if not condition:
        raise AssertionError(label)
    print('PASS:', label, flush=True)


def make_config(folder: Path) -> tuple[Path, int]:
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    # Other two peers are not used, but must have valid unique ports.
    ports = [port, 1, 2] if port > 2 else [port, 3, 4]
    config = folder / 'node.conf'
    config.write_text('node_id=1\n' + ''.join(
        f'peer={i},127.0.0.1,{p}\n' for i, p in enumerate(ports, 1)),
        encoding='ascii')
    return config, port


class Server:
    def __init__(self, binary: str, config: Path, max_connections: int | None = None):
        args = [binary, '--serve', str(config)]
        if max_connections is not None:
            args += ['--max-connections', str(max_connections)]
        self.proc = subprocess.Popen(args, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, bufsize=0)
        self.lines = bytearray()
        self.wait_for(b'Listening: ', 3.0)

    def wait_for(self, marker: bytes, seconds: float) -> None:
        deadline = time.monotonic() + seconds
        assert self.proc.stdout is not None
        while marker not in self.lines:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(f'timed out waiting for {marker!r}; output={self.lines!r}')
            ready, _, _ = select.select([self.proc.stdout], [], [], remaining)
            if not ready:
                continue
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            if not chunk:
                raise AssertionError('server exited while awaiting output')
            self.lines.extend(chunk)

    def wait_accepted(self, n: int, seconds: float = 3.0) -> None:
        self.wait_for(f'connection={n} '.encode('ascii'), seconds)

    def done(self, seconds: float = 5.0) -> tuple[bytes, bytes]:
        out, err = self.proc.communicate(timeout=seconds)
        return bytes(self.lines) + out, err

    def stop(self) -> tuple[bytes, bytes]:
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
        try:
            return self.done(5.0)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.communicate(timeout=5)
            raise AssertionError('server did not shut down in five seconds')


def hold_partial(port: int) -> socket.socket:
    sock = socket.create_connection(('127.0.0.1', port), timeout=2)
    sock.settimeout(2)
    sock.sendall(b'R')  # only first byte of wire header: worker blocks in recv
    return sock


def run_ping(binary: str, config: Path, message: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([binary, '--ping', str(config), '1', message],
                          capture_output=True, text=True, timeout=4)


def main(binary: str) -> None:
    binary = str(Path(binary).resolve())
    with tempfile.TemporaryDirectory(prefix='raft-phase06-') as folder:
        base = Path(folder)
        # One deliberately slow client must not delay six fast requests.
        config, port = make_config(base)
        server = Server(binary, config, max_connections=7)
        slow = None
        stop_trickle = threading.Event()
        trickle_thread = None
        try:
            slow = hold_partial(port)
            server.wait_accepted(1)
            # Keep partial recv() active for ~2 seconds. A sequential server
            # would remain head-of-line blocked; the threaded server does not.
            def drip() -> None:
                for _ in range(8):
                    if stop_trickle.wait(0.25):
                        return
                    try:
                        slow.sendall(b'X')
                    except OSError:
                        return
            trickle_thread = threading.Thread(target=drip, daemon=True)
            trickle_thread.start()
            started = time.monotonic()
            with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
                futures = [pool.submit(run_ping, binary, config, f'parallel-{i}')
                           for i in range(6)]
                replies = [f.result(timeout=4) for f in futures]
            elapsed = time.monotonic() - started
            check(all(r.returncode == 0 and 'PONG:' in r.stdout for r in replies),
                  'six parallel PING requests get valid PONG responses')
            check(elapsed < 1.65, 'fast clients finish while one client is stalled')
            stop_trickle.set()
            trickle_thread.join(timeout=2)
            slow.close()
            slow = None
            out, err = server.done()
            check(server.proc.returncode == 0 and b'Stopped: node=1 accepted=7' in out,
                  f'bounded server drains all seven connections ({err[:200]!r})')
        finally:
            stop_trickle.set()
            if trickle_thread is not None:
                trickle_thread.join(timeout=2)
            if slow is not None:
                slow.close()
            if server.proc.poll() is None:
                server.stop()

        # All eight workers blocked. The next client waits in the listen queue,
        # then is served when a slot becomes available (bounded concurrency).
        config2, port2 = make_config(base)
        server = Server(binary, config2, max_connections=9)
        held: list[socket.socket] = []
        queued = None
        try:
            for n in range(WORKERS):
                held.append(hold_partial(port2))
                server.wait_accepted(n + 1)
            queued = subprocess.Popen(
                [binary, '--ping', str(config2), '1', 'queued'],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            time.sleep(0.2)
            check(queued.poll() is None,
                  'ninth request waits rather than creating an unbounded thread')
            # Truncated frame frees the first worker; queued request can proceed.
            held[0].close()
            held.pop(0)
            stdout, stderr = queued.communicate(timeout=4)
            check(queued.returncode == 0 and 'PONG:' in stdout,
                  f'queued request completes when worker slot becomes available ({stderr})')
            for sock in held:
                sock.close()
            held.clear()
            out, err = server.done()
            check(server.proc.returncode == 0 and b'Stopped: node=1 accepted=9' in out,
                  f'worker pool remains bounded and drains ({err[:200]!r})')
        finally:
            for sock in held:
                sock.close()
            if queued is not None and queued.poll() is None:
                queued.kill()
                queued.communicate(timeout=5)
            if server.proc.poll() is None:
                server.stop()

        # SIGTERM should interrupt hung workers and join them safely.
        config3, port3 = make_config(base)
        server = Server(binary, config3)
        sockets: list[socket.socket] = []
        try:
            for n in range(4):
                sockets.append(hold_partial(port3))
                server.wait_accepted(n + 1)
            begun = time.monotonic()
            out, err = server.stop()
            duration = time.monotonic() - begun
            check(server.proc.returncode == 0 and b'Stopped: node=1 accepted=4' in out,
                  f'SIGTERM joins active workers without crashing ({err[:200]!r})')
            check(duration < 2.0, 'signal-driven shutdown does not wait for idle timeout')
        finally:
            for sock in sockets:
                sock.close()
            if server.proc.poll() is None:
                server.stop()

    print('All Phase 6 concurrent TCP integration tests passed.')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: test_concurrent_tcp.py PATH_TO_BINARY')
    main(sys.argv[1])
