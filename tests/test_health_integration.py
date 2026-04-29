#!/usr/bin/env python3
"""Phase 29: liveness vs operational readiness, quorum loss and mTLS controls."""
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


def run(binary, *args, timeout=8):
    return subprocess.run([binary, *map(str, args)], text=True,
                          capture_output=True, timeout=timeout)


def stop(proc):
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill(); proc.wait(timeout=4)


def health(binary, config, node, tls=None):
    args = ('--health-tls', config, node, *tls) if tls else ('--health', config, node)
    result = run(binary, *args)
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert result.stdout.startswith(f'Health: node={node} live=1 '), result.stdout
    fields = dict(part.split('=', 1) for part in result.stdout.strip().split()[1:]
                  if '=' in part)
    return fields


def ready(binary, config, tls=None):
    args = ('--ready-tls', config, *tls) if tls else ('--ready', config)
    result = run(binary, *args, timeout=12)
    assert result.returncode in (0, 2), (result.stdout, result.stderr)
    assert result.stdout.startswith('Readiness: ready='), (result.stdout, result.stderr)
    return result.returncode == 0, result.stdout


def plain_cluster(binary, root):
    ports = support.ports_three()
    configs=[]
    for n in (1, 2, 3):
        p=root/f'node-{n}.conf'
        p.write_text('node_id='+str(n)+'\n'+''.join(
            f'peer={j},127.0.0.1,{port}\n' for j,port in enumerate(ports,1)))
        configs.append(p)
    # A standalone listener is live but cannot serve Raft client writes.
    standalone_log=open(root/'standalone.log','w+')
    standalone=subprocess.Popen([binary,'--serve',str(configs[0])],
                                stdout=standalone_log,stderr=subprocess.STDOUT)
    try:
        def standalone_ready():
            try: return health(binary,configs[0],1)
            except (AssertionError,subprocess.TimeoutExpired): return None
        sample=support.wait_for(standalone_ready,8)
        assert sample['write_ready']=='0' and sample['elections']=='0'
        ok, _=ready(binary,configs[0]); assert not ok
        print('PASS health: standalone server live, not write-ready')
        # Malformed health request (nonempty payload) must not receive a reply.
        with socket.create_connection(('127.0.0.1',ports[0]),timeout=2) as sock:
            sock.settimeout(2)
            sock.sendall(struct.pack('!IBBHI',0x52414654,1,29,0,1)+b'!')
            try: assert sock.recv(12) == b''
            except ConnectionResetError: pass
        sample=health(binary,configs[0],1)
        assert sample['live']=='1'
        print('PASS health: malformed request rejected without poisoning listener')
    finally:
        stop(standalone); standalone_log.close()
    procs=[None,None,None]; logs=[]
    try:
        for n in (1,2,3):
            out=open(root/f'node-{n}.log','w+')
            logs.append(out)
            procs[n-1]=subprocess.Popen([binary,'--elect',str(configs[n-1])],
                                        stdout=out,stderr=subprocess.STDOUT)
            time.sleep(.14)
        def stable_leader():
            try:
                samples={n:health(binary,configs[0],n) for n in (1,2,3)}
                leaders=[n for n,v in samples.items() if v['role']=='leader'
                         and v['write_ready']=='1']
                if len(leaders)!=1 or len({v['term'] for v in samples.values()})!=1:
                    return None
                return leaders[0]
            except (AssertionError,subprocess.TimeoutExpired,RuntimeError):
                return None
        leader=support.wait_for(stable_leader,18)
        h=health(binary,configs[0],leader)
        assert h['storage_ok']=='1' and h['quorum_recent']=='1'
        assert h['quorum_age_ms']!='unknown'
        for n in (1,2,3):
            if n!=leader: assert health(binary,configs[0],n)['write_ready']=='0'
        ok, desc=ready(binary,configs[0]); assert ok and f'leader={leader}' in desc
        print('PASS health: three-node election, one write-ready leader, cluster readiness')

        # One disconnected follower still leaves a two-voter majority.
        followers=[n for n in (1,2,3) if n!=leader]
        stop(procs[followers[0]-1])
        support.wait_for(lambda: ready(binary,configs[0])[0],12)
        assert health(binary,configs[0],leader)['write_ready']=='1'
        print('PASS health: one unavailable follower does not remove write readiness')

        # Isolated leader remains TCP-live, but its recent ACK expires.
        stop(procs[followers[1]-1])
        def lost():
            try:
                sample=health(binary,configs[0],leader)
                return sample if sample['write_ready']=='0' else None
            except (AssertionError,subprocess.TimeoutExpired): return None
        sample=support.wait_for(lost,14)
        assert sample['live']=='1' and sample['quorum_recent']=='0'
        ok, desc=ready(binary,configs[0]); assert not ok, desc
        print('PASS health: isolated leader remains live, write readiness expires')

        # Restore quorum and wait for a new valid same-term acknowledgement.
        n=followers[0]
        procs[n-1]=subprocess.Popen([binary,'--elect',str(configs[n-1])],
                                   stdout=logs[n-1],stderr=subprocess.STDOUT)
        support.wait_for(lambda: ready(binary,configs[0])[0],18)
        print('PASS health: readiness recovers when a voting follower returns')
    finally:
        for proc in procs:
            if proc: stop(proc)
        for output in logs: output.close()


def tls_endpoint(binary, root):
    root.mkdir()
    (root/'ca-ext').write_text('basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n')
    ca,cakey=tls_support.certificate(root,'ca',True)
    nodecert,nodekey=tls_support.certificate(root,'raft-node-1',issuer=(ca,cakey))
    clientcert,clientkey=tls_support.certificate(root,'raft-client',issuer=(ca,cakey))
    outsider,outsiderkey=tls_support.certificate(root,'raft-outsider',issuer=(ca,cakey))
    ports=support.ports_three()
    config=root/'config.conf'
    config.write_text('node_id=1\n'+''.join(
        f'peer={i},127.0.0.1,{p}\n' for i,p in enumerate(ports,1)))
    keyring=root/'keys'
    keyring.write_text(f'71001 {secrets.token_hex(32)}\n'); keyring.chmod(0o600)
    storage=root/'storage'; storage.mkdir()
    out=open(root/'tls.log','w+')
    proc=subprocess.Popen([binary,'--elect-tls-auth-wal',str(config),str(storage),
                           str(keyring),str(nodecert),str(nodekey),str(ca)],
                          stdout=out,stderr=subprocess.STDOUT)
    tls=(clientcert,clientkey,ca)
    try:
        def tls_ready():
            try: return health(binary,config,1,tls)
            except (AssertionError,subprocess.TimeoutExpired): return None
        support.wait_for(tls_ready,18)
        sample=health(binary,config,1,tls)
        assert sample['write_ready']=='0' # no quorum; TLS does not bypass consensus
        assert run(binary,'--health',config,'1').returncode != 0
        assert run(binary,'--health-tls',config,'1',outsider,outsiderkey,ca).returncode != 0
        ok, _=ready(binary,config,tls); assert not ok
        print('PASS health: mTLS read access, plaintext and outsider rejected; no false readiness')
    finally:
        stop(proc); out.close()


def main(binary):
    binary=str(Path(binary).resolve())
    with tempfile.TemporaryDirectory(prefix='raft-health-29-') as d:
        root=Path(d)
        plain_cluster(binary,root)
        tls_endpoint(binary,root/'tls')
    print('PASS Phase 29 operational health integration')


if __name__=='__main__':
    main(sys.argv[1] if len(sys.argv)>1 else './build/raft-engine')
