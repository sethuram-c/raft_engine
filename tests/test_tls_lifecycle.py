#!/usr/bin/env python3
"""Phase 27: hot mTLS certificate rotation and bounded handshake resources."""
import hashlib
import importlib.util
import os
from pathlib import Path
import secrets
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import time

spec = importlib.util.spec_from_file_location('old_tls', Path(__file__).with_name('test_tls_integration.py'))
old_tls = importlib.util.module_from_spec(spec)
assert spec and spec.loader
spec.loader.exec_module(old_tls)
support = old_tls.support


def replace_pem(source, destination):
    """Simulate provisioner replacing one PEM file atomically."""
    tmp = destination.with_name(destination.name + '.next')
    tmp.write_bytes(source.read_bytes())
    tmp.chmod(0o600 if destination.suffix == '.key' else 0o644)
    os.replace(tmp, destination)


def wait_for(predicate, seconds=10):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        try:
            value = predicate()
            if value:
                return value
        except (OSError, ssl.SSLError, subprocess.TimeoutExpired):
            pass
        time.sleep(.07)
    raise AssertionError('condition not met by timeout')


def main(binary):
    binary = str(Path(binary).resolve())
    with tempfile.TemporaryDirectory(prefix='raft-phase27-') as td:
        root = Path(td)
        (root/'ca-ext').write_text('basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n')
        ca, ca_key = old_tls.certificate(root, 'ca27', True)
        nodes = {n: old_tls.certificate(root, f'raft-node-{n}', issuer=(ca,ca_key)) for n in (1,2,3)}
        client, client_key = old_tls.certificate(root, 'raft-client', issuer=(ca,ca_key))
        renewed, renewed_key = old_tls.certificate(root, 'renewed', issuer=(ca,ca_key))
        # Renew the leaf with the original raft-node-1 SAN and a fresh key.
        renewed_ext = root/'renewed.ext'
        renewed_ext.write_text('subjectAltName=DNS:raft-node-1\nextendedKeyUsage=serverAuth,clientAuth\nbasicConstraints=critical,CA:FALSE\n')
        old_tls.run('openssl','x509','-req','-in',str(root/'renewed.csr'),
            '-CA',str(ca),'-CAkey',str(ca_key),'-CAcreateserial','-out',str(renewed),
            '-days','2','-sha256','-extfile',str(renewed_ext))
        wrong, wrong_key = old_tls.certificate(root,'raft-outsider',issuer=(ca,ca_key))
        ports = support.ports_three()
        configs = []
        for n in (1,2,3):
            cfg = root/f'node{n}.conf'
            cfg.write_text(f'node_id={n}\n' + ''.join(
                f'peer={i},127.0.0.1,{port}\n' for i,port in enumerate(ports, 1)))
            configs.append(cfg)
        key = secrets.token_bytes(32)
        secret = root/'client.secret';secret.write_text(key.hex()+'\n');secret.chmod(0o600)
        keyring = root/'clients.keys';keyring.write_text(f'71001 {key.hex()}\n');keyring.chmod(0o600)
        data = root/'data';data.mkdir()
        procs={};outputs=[]
        def start(n):
            out=open(root/f'node-{n}-{len(outputs)}.log','w+')
            outputs.append(out)
            cert,priv=nodes[n]
            procs[n]=subprocess.Popen([binary,'--elect-tls-auth-wal',str(configs[n-1]),
                str(data),str(keyring),str(cert),str(priv),str(ca)],
                stdout=out,stderr=subprocess.STDOUT)
        def stop(n):
            process=procs.get(n)
            if process and process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:process.wait(timeout=5)
                except subprocess.TimeoutExpired:process.kill();process.wait(timeout=3)
        def status(n):
            return subprocess.run([binary,'--status-tls',str(configs[0]),str(n),
                str(client),str(client_key),str(ca)],capture_output=True,text=True,timeout=5)
        def write(id):
            return subprocess.run([binary,'--write-tls-auth-auto',str(configs[0]),
                '10000','71001',str(id),str(secret),str(client),str(client_key),str(ca),
                'SET rotation verified'],capture_output=True,text=True,timeout=19)
        tls_ctx=ssl.create_default_context(cafile=str(ca))
        tls_ctx.load_cert_chain(str(client),str(client_key))
        def fingerprint(n):
            raw=socket.create_connection(('127.0.0.1',ports[n-1]),timeout=2)
            with tls_ctx.wrap_socket(raw,server_hostname=f'raft-node-{n}') as conn:
                return hashlib.sha256(conn.getpeercert(binary_form=True)).hexdigest()
        try:
            start(1);time.sleep(.16);start(2);time.sleep(.16);start(3)
            wait_for(lambda: all(status(n).returncode==0 for n in (1,2,3)), 17)
            first=write(1)
            assert first.returncode==0 and 'COMMITTED' in first.stdout, first
            old_fp=fingerprint(1)
            expected_new=hashlib.sha256(ssl.PEM_cert_to_DER_cert(renewed.read_text())).hexdigest()
            assert old_fp!=expected_new
            # Invalid certificate is rejected without unloading old key material.
            replace_pem(wrong,nodes[1][0]);replace_pem(wrong_key,nodes[1][1])
            procs[1].send_signal(signal.SIGHUP)
            time.sleep(.35)
            assert fingerprint(1)==old_fp
            assert procs[1].poll() is None
            print('PASS lifecycle: invalid rotated SAN rejected; old TLS context remains active')
            # Successful leaf/key pair replacement without process restart.
            replace_pem(renewed,nodes[1][0]);replace_pem(renewed_key,nodes[1][1])
            procs[1].send_signal(signal.SIGHUP)
            wait_for(lambda: fingerprint(1)==expected_new, 7)
            assert procs[1].poll() is None
            print('PASS lifecycle: SIGHUP atomically activates renewed identity with no restart')
            # Missing or over-permissive keys cannot replace a good context.
            nodes[1][1].chmod(0o644)
            procs[1].send_signal(signal.SIGHUP)
            time.sleep(.3)
            assert fingerprint(1)==expected_new
            nodes[1][1].chmod(0o600)
            print('PASS lifecycle: owner-only key validation and fail-closed reload')
            # Eight incomplete TLS handshakes temporarily occupy all slots.
            # A single overall handshake deadline must eventually free them.
            hanging=[]
            for _ in range(8):
                try:
                    s=socket.create_connection(('127.0.0.1',ports[0]),timeout=1)
                    hanging.append(s)
                except OSError: pass
            assert len(hanging)>=6,len(hanging)
            time.sleep(1.8)
            wait_for(lambda: status(1).returncode==0, 7)
            print('PASS lifecycle: idle TLS handshakes expire; worker slots become reusable')
            for s in hanging:s.close()
            # Cluster remains available and deduplication still works.
            result=write(2)
            assert result.returncode==0 and 'COMMITTED' in result.stdout,(result.stdout,result.stderr)
            retry=write(2)
            assert retry.returncode==0 and 'COMMITTED' in retry.stdout,(retry.stdout,retry.stderr)
            print('PASS lifecycle: cluster remains writable and retries stay idempotent')
        finally:
            for n in (1,2,3):stop(n)
            for out in outputs:out.close()
            if os.getenv('RAFT_TLS_LIFECYCLE_DEBUG'):
                for log in root.glob('node-*.log'):
                    print(f'--- {log.name} ---\n{log.read_text()[-6000:]}')

if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv)>1 else 'build/raft-engine')
