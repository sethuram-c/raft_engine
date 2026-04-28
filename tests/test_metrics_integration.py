#!/usr/bin/env python3
"""Phase 28: live metrics, election/write counters, resource bounds and mTLS."""
import importlib.util
from pathlib import Path
import secrets
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('progress_support', HERE/'test_progress_integration.py')
support = importlib.util.module_from_spec(spec)
assert spec and spec.loader
spec.loader.exec_module(support)
spec2 = importlib.util.spec_from_file_location('tls_support', HERE/'test_tls_integration.py')
tls_support = importlib.util.module_from_spec(spec2)
assert spec2 and spec2.loader
spec2.loader.exec_module(tls_support)


def run(binary, *args, timeout=6):
    return subprocess.run([binary, *map(str, args)], capture_output=True,
                          text=True, timeout=timeout)


def metrics(binary, config, node, tls=None):
    args = ('--metrics-tls', config, node, *tls) if tls else ('--metrics', config, node)
    result = run(binary, *args)
    if result.returncode:
        raise RuntimeError(f'metrics failed: {result.stdout}\n{result.stderr}')
    assert result.stdout.startswith(f'Metrics: node={node} source=live volatile=1\n')
    values = {}
    for line in result.stdout.splitlines()[1:]:
        key, value = line.split(' ')
        assert key.startswith('raft_') and key not in values
        values[key[5:]] = int(value)
    assert len(values) == 29, values
    assert 0 <= values['connections_active'] <= values['connections_peak'] <= 8
    assert values['last_applied'] <= values['commit_index'] <= values['last_log_index']
    return values


def stop(process, hard=False):
    if process.poll() is None:
        if hard: process.kill()
        else: process.send_signal(signal.SIGTERM)
        try: process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait(timeout=3)


def plain_cluster(binary, root):
    ports = support.ports_three()
    configs = []
    for n in (1, 2, 3):
        p = root/f'plain-{n}.conf'
        p.write_text(f'node_id={n}\n' + ''.join(
            f'peer={i},127.0.0.1,{port}\n' for i,port in enumerate(ports,1)))
        configs.append(p)
    procs = []
    outputs = []
    try:
        for n in (1, 2, 3):
            output = open(root/f'plain-{n}.log', 'w+')
            outputs.append(output)
            procs.append(subprocess.Popen([binary,'--elect',str(configs[n-1])],
                                          stdout=output,stderr=subprocess.STDOUT))
            time.sleep(.13)
        def elected():
            try:
                data = {n: metrics(binary, configs[0], n) for n in (1,2,3)}
            except (RuntimeError, AssertionError, subprocess.TimeoutExpired):
                return None
            leaders = [n for n,v in data.items() if v['role'] == 2]
            if len(leaders) != 1 or len({v['current_term'] for v in data.values()}) != 1:
                return None
            return leaders[0]
        leader = support.wait_for(elected, 18)
        first = metrics(binary, configs[0], leader)
        assert first['leader_transitions_total'] >= 1
        assert first['election_rounds_total'] >= 1
        assert first['replication_rpc_successes_total'] >= 1
        assert first['connections_accepted_total'] >= 1
        print('PASS metrics: live elections, leadership and heartbeat counters')

        result = run(binary, '--write-auto', configs[0], '10000', '92801', '1',
                     'SET observability working', timeout=16)
        assert result.returncode == 0 and 'COMMITTED' in result.stdout, (result.stdout, result.stderr)
        support.wait_for(lambda: all(metrics(binary, configs[0], n)['last_applied'] >= 1
                                     for n in (1,2,3)), 12)
        later = metrics(binary, configs[0], leader)
        assert later['last_log_index'] >= 1 and later['commit_index'] >= 1
        assert later['client_commits_total'] >= 1
        assert later['entries_sent_total'] >= 1
        print('PASS metrics: committed write, applied indices and entries sent')

        # One follower goes offline; two-node quorum must keep writing.
        offline = next(n for n in (1,2,3) if n != leader)
        stop(procs[offline-1])
        second = run(binary,'--write-auto',configs[0],'9000','92801','2',
                     'SET observability recovered',timeout=15)
        assert second.returncode == 0 and 'COMMITTED' in second.stdout, (second.stdout,second.stderr)
        support.wait_for(lambda: metrics(binary, configs[0], leader)['replication_rpc_failures_total'] > 0, 9)
        degraded = metrics(binary, configs[0], leader)
        assert degraded['max_follower_lag'] >= 1, degraded
        assert degraded['commit_index'] >= 2
        print('PASS metrics: failed peer RPCs and follower lag during one-node outage')

        # Invalid length is rejected before message decoding.
        with socket.create_connection(('127.0.0.1', ports[leader-1]), timeout=2) as sock:
            sock.settimeout(2)
            sock.sendall(struct.pack('!IBBHI',0x52414654,1,27,0,1) + b'X')
            try:
                bad = sock.recv(12)
                assert not bad, bad
            except ConnectionResetError:
                pass
        support.wait_for(lambda: metrics(binary, configs[0], leader)['rejected_frames_total'] >= 1, 6)
        after = metrics(binary, configs[0], leader)
        assert after['connections_peak'] <= 8
        assert after['connections_accepted_total'] >= first['connections_accepted_total']
        print('PASS metrics: malformed metrics request rejected, bounded connection gauges')
        assert 'client_id' not in str(after) and 'secret' not in str(after)
    finally:
        for proc in procs: stop(proc)
        for out in outputs: out.close()


def tls_endpoint(binary, root):
    root.mkdir()
    (root/'ca-ext').write_text('basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n')
    ca, ca_key = tls_support.certificate(root, 'ca', True)
    nodecert, nodekey = tls_support.certificate(root,'raft-node-1',issuer=(ca,ca_key))
    clientcert, clientkey = tls_support.certificate(root,'raft-client',issuer=(ca,ca_key))
    wrongcert, wrongkey = tls_support.certificate(root,'raft-outsider',issuer=(ca,ca_key))
    ports = support.ports_three()
    conf = root/'node-1.conf'
    conf.write_text('node_id=1\n'+''.join(
        f'peer={i},127.0.0.1,{p}\n' for i,p in enumerate(ports,1)))
    keyring = root/'keys'
    keyring.write_text(f'71001 {secrets.token_hex(32)}\n'); keyring.chmod(0o600)
    data = root/'storage'; data.mkdir()
    out = open(root/'tls-node.log','w+')
    proc = subprocess.Popen([binary,'--elect-tls-auth-wal',str(conf),str(data),
                             str(keyring),str(nodecert),str(nodekey),str(ca)],
                            stdout=out,stderr=subprocess.STDOUT)
    try:
        def tls_ready():
            try:
                return metrics(binary,conf,1,(clientcert,clientkey,ca))
            except (RuntimeError, AssertionError, subprocess.TimeoutExpired):
                return None
        support.wait_for(tls_ready, 17)
        ok = metrics(binary,conf,1,(clientcert,clientkey,ca))
        assert ok['node_id']==1
        assert run(binary,'--metrics',conf,'1').returncode != 0
        assert run(binary,'--metrics-tls',conf,'1',wrongcert,wrongkey,ca).returncode != 0
        assert run(binary,'--metrics-tls',conf,'1',clientcert,clientkey,ca).returncode == 0
        print('PASS metrics: client mTLS required, plaintext and outsider certificates rejected')
        before = metrics(binary,conf,1,(clientcert,clientkey,ca))
        assert before['tls_handshake_failures_total'] >= 1
        print('PASS metrics: TLS handshake failure counter increases')
    finally:
        stop(proc)
        out.close()


def main(binary):
    binary = str(Path(binary).resolve())
    with tempfile.TemporaryDirectory(prefix='raft-metrics-28-') as directory:
        root=Path(directory)
        plain_cluster(binary,root)
        tls_endpoint(binary,root/'tls')
    print('PASS Phase 28 metrics integration')


if __name__=='__main__':
    main(sys.argv[1] if len(sys.argv)>1 else './build/raft-engine')
