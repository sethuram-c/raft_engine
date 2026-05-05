#!/usr/bin/env python3
"""Phase 35: cross-feature capstone in a disposable, mutually authenticated cluster.

The test intentionally uses public CLI surfaces, not direct calls into C internals.
Secrets/certificates and node data live only in a temporary private directory.
"""
from __future__ import annotations

import importlib.util
from pathlib import Path
import re
import secrets
import signal
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent


def load_support(name: str):
    spec = importlib.util.spec_from_file_location(name, HERE / (name + '.py'))
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


support = load_support('test_progress_integration')
tls_support = load_support('test_tls_integration')


def run(binary: str, *args: object, timeout: float = 10) -> subprocess.CompletedProcess:
    return subprocess.run([binary, *map(str, args)], capture_output=True,
                          text=True, timeout=timeout)


def parse_index(output: str) -> int:
    assert 'status=COMMITTED' in output, output
    match = re.search(r'\bindex=(\d+)\b', output)
    assert match, output
    return int(match.group(1))


def parse_metric(output: str, name: str) -> int:
    match = re.search(rf'(?m)^raft_{re.escape(name)} (\d+)$', output)
    assert match, (name, output)
    return int(match.group(1))


def main(binary: str) -> None:
    binary = str(Path(binary).resolve())
    with tempfile.TemporaryDirectory(prefix='raft-phase35-') as temporary:
        root = Path(temporary)
        (root / 'ca-ext').write_text(
            'basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n')
        ca, ca_key = tls_support.certificate(root, 'capstone-ca', True)
        certs = {n: tls_support.certificate(root, f'raft-node-{n}',
                                           issuer=(ca, ca_key)) for n in (1, 2, 3)}
        client, client_key = tls_support.certificate(root, 'raft-client', issuer=(ca, ca_key))
        outsider, outsider_key = tls_support.certificate(root, 'raft-outsider', issuer=(ca, ca_key))
        identity = (client, client_key, ca)
        ports = support.ports_three()
        configs = []
        for n in (1, 2, 3):
            cfg = root / f'node-{n}.conf'
            cfg.write_text(f'node_id={n}\n' + ''.join(
                f'peer={i},127.0.0.1,{port}\n' for i, port in enumerate(ports, 1)))
            configs.append(cfg)
        client_id = '35001'
        barrier_client = '35002'
        key = secrets.token_hex(32)
        barrier_key = secrets.token_hex(32)
        secret = root / 'client.hex'
        secret.write_text(key + '\n')
        secret.chmod(0o600)
        keyring = root / 'server.keys'
        keyring.write_text(f'{client_id} {key}\n{barrier_client} {barrier_key}\n')
        keyring.chmod(0o600)
        barrier_secret = root / 'barrier-client.hex'
        barrier_secret.write_text(barrier_key + '\n')
        barrier_secret.chmod(0o600)
        data = root / 'storage'
        data.mkdir(mode=0o700)
        procs = {}
        streams = []

        def start(n: int) -> None:
            output = (root / f'node-{n}-{len(streams)}.log').open('w+')
            streams.append(output)
            cert, private = certs[n]
            procs[n] = subprocess.Popen(
                [binary, '--elect-tls-auth-wal', str(configs[n-1]), str(data),
                 str(keyring), str(cert), str(private), str(ca)],
                stdout=output, stderr=subprocess.STDOUT)

        def stop(n: int, hard: bool = False) -> None:
            proc = procs.get(n)
            if proc is None or proc.poll() is not None:
                return
            proc.send_signal(signal.SIGKILL if hard else signal.SIGTERM)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=4)

        def query(command: str, node: int, *credentials):
            return run(binary, command, configs[0], node,
                       *(credentials or identity), timeout=6)

        def status(n: int):
            result = query('--status-tls', n)
            if result.returncode:
                return None
            match = re.search(r'\brole=(follower|candidate|leader)\b.*\bterm=(\d+)',
                              result.stdout)
            # Fields are printed in this order in the Phase 26 CLI.
            if not match:
                role = re.search(r'\brole=(follower|candidate|leader)\b', result.stdout)
                term = re.search(r'\bterm=(\d+)\b', result.stdout)
                if not role or not term:
                    return None
                return role.group(1), int(term.group(1))
            return match.group(1), int(match.group(2))

        def elected():
            try:
                states = {n: status(n) for n in (1, 2, 3)
                          if procs[n].poll() is None}
                if any(s is None for s in states.values()):
                    return None
                leaders = [n for n, s in states.items() if s[0] == 'leader']
                return leaders[0] if len(leaders) == 1 else None
            except (OSError, subprocess.TimeoutExpired):
                return None

        def ready():
            result = run(binary, '--ready-tls', configs[0], *identity, timeout=12)
            return result if result.returncode == 0 and 'ready=1' in result.stdout else None

        def write(request_id: int, command: str, budget: int = 10000,
                  use_barrier_client: bool = False):
            return run(binary, '--write-tls-auth-auto', configs[0], budget,
                       barrier_client if use_barrier_client else client_id, request_id,
                       barrier_secret if use_barrier_client else secret, *identity, command,
                       timeout=budget / 1000 + 14)

        def committed(request_id: int, command: str) -> int:
            result = write(request_id, command)
            assert result.returncode == 0, (result.stdout, result.stderr)
            return parse_index(result.stdout)

        def metrics(n: int):
            result = query('--metrics-tls', n)
            assert result.returncode == 0, (result.stdout, result.stderr)
            return result.stdout

        try:
            for n in (1, 2, 3):
                start(n)
                time.sleep(0.19)
            leader = support.wait_for(elected, 22)
            support.wait_for(ready, 16)
            print('PASS capstone: mTLS cluster elects one writable leader')

            invalid = query('--health-tls', leader, outsider, outsider_key, ca)
            assert invalid.returncode != 0, (invalid.stdout, invalid.stderr)
            print('PASS capstone: unauthorized certificate rejected by operational RPC')

            first_index = committed(1, 'SET capstone first')
            again_index = committed(1, 'SET capstone first')
            assert first_index > 0 and again_index == first_index
            rejected = write(1, 'SET capstone forged', budget=2500)
            assert rejected.returncode != 0 and 'REJECTED' in rejected.stdout, rejected
            print('PASS capstone: committed write, same-ID retry, conflicting reuse rejection')

            initial = metrics(leader)
            assert parse_metric(initial, 'commit_index') >= first_index
            assert parse_metric(initial, 'last_applied') <= parse_metric(initial, 'commit_index')
            assert parse_metric(initial, 'connections_active') <= 8
            print('PASS capstone: metrics preserve log/apply ordering and worker bounds')

            offline = next(n for n in (1, 2, 3) if n != leader)
            stop(offline, hard=True)
            second_index = committed(2, 'SET capstone quorum')
            assert second_index > first_index
            assert (data / f'node-{leader}.snapwal').is_file()
            print('PASS capstone: one follower down; surviving majority commits durably')

            start(offline)
            def caught_up():
                try:
                    observed = metrics(offline)
                    if parse_metric(observed, 'last_applied') >= second_index:
                        return True
                except (AssertionError, subprocess.TimeoutExpired):
                    return None
                return None
            support.wait_for(caught_up, 20)
            print('PASS capstone: restarted lagging follower catches up and applies entries')

            for n in (1, 2, 3):
                stop(n, hard=True)
            for n in (1, 2, 3):
                start(n)
                time.sleep(0.2)
            support.wait_for(elected, 25)
            support.wait_for(ready, 17)
            # A current-term write is a safe commitment barrier after a new election.
            # A prior-term entry may not advance commitIndex directly on its own.
            barrier = write(1, 'SET capstone after-restart', use_barrier_client=True)
            assert barrier.returncode == 0, (barrier.stdout, barrier.stderr)
            barrier_index = parse_index(barrier.stdout)
            assert barrier_index > second_index
            restored_index = committed(2, 'SET capstone quorum')
            assert restored_index == second_index, (second_index, restored_index)
            print('PASS capstone: SIGKILL/restart and separate-client term barrier retain last request ID')
            assert sum(1 for n in (1, 2, 3) if (data / f'node-{n}.snapwal').is_file()) == 3
            print('PASS capstone: all three node-specific durable WAL files recovered')
        except Exception:
            for output in streams:
                output.flush()
                print(f'--- {output.name} ---', file=sys.stderr)
                print(Path(output.name).read_text()[-2000:], file=sys.stderr)
            raise
        finally:
            for n in (1, 2, 3):
                stop(n, hard=True)
            for output in streams:
                output.close()


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'build/raft-engine')
