#!/usr/bin/env python3
"""Phase 26: genuine TLS 1.3 mutual authentication, identity binding, HMAC writes.
Generates throwaway certificate authority and node/client leaves in a temp dir.
"""
import importlib.util
import os
from pathlib import Path
import secrets
import ssl
import struct
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time

spec=importlib.util.spec_from_file_location('support',Path(__file__).with_name('test_progress_integration.py'))
support=importlib.util.module_from_spec(spec)
assert spec and spec.loader
spec.loader.exec_module(support)

def run(*args):
    p=subprocess.run(args, stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=12)
    if p.returncode: raise AssertionError(f'openssl failed: {args}\n{p.stderr}')

def certificate(root, name, ca=False, issuer=None):
    key=root/f'{name}.key';cert=root/f'{name}.crt'
    run('openssl','req','-new','-newkey','rsa:2048','-nodes','-keyout',str(key),
        '-out',str(root/f'{name}.csr'),'-subj',f'/CN={name}')
    if ca:
        run('openssl','x509','-req','-in',str(root/f'{name}.csr'),'-signkey',str(key),
            '-out',str(cert),'-days','2','-sha256','-extfile',str(root/'ca-ext'))
    else:
        ext=root/f'{name}.ext'
        ext.write_text('subjectAltName=DNS:'+name+'\n'
                       'extendedKeyUsage=serverAuth,clientAuth\n'
                       'basicConstraints=critical,CA:FALSE\n')
        run('openssl','x509','-req','-in',str(root/f'{name}.csr'),
            '-CA',str(issuer[0]),'-CAkey',str(issuer[1]),'-CAcreateserial',
            '-out',str(cert),'-days','2','-sha256','-extfile',str(ext))
    key.chmod(0o600)
    return cert,key

def main(binary):
    binary=str(Path(binary).resolve())
    with tempfile.TemporaryDirectory(prefix='raft-phase26-') as td:
        root=Path(td)
        (root/'ca-ext').write_text('basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n')
        ca,ca_key=certificate(root,'test-ca',True)
        node={n:certificate(root,f'raft-node-{n}',issuer=(ca,ca_key)) for n in (1,2,3)}
        client,client_key=certificate(root,'raft-client',issuer=(ca,ca_key))
        outsider,outsider_key=certificate(root,'raft-outsider',issuer=(ca,ca_key))
        wrong_ca,wrong_key=certificate(root,'wrong-ca',True)
        wrong_client,wrong_client_key=certificate(root,'raft-client-wrong',issuer=(wrong_ca,wrong_key))
        ports=support.ports_three()
        cfgs=[]
        for n in (1,2,3):
            cfg=root/f'node{n}.conf'
            cfg.write_text(f'node_id={n}\n'+''.join(f'peer={i},127.0.0.1,{p}\n' for i,p in enumerate(ports,1)))
            cfgs.append(cfg)
        key=secrets.token_bytes(32)
        secret=root/'client-secret.hex';secret.write_text(key.hex()+'\n');secret.chmod(0o600)
        keyring=root/'server.keyring';keyring.write_text(f'71001 {key.hex()}\n');keyring.chmod(0o600)
        data=root/'data';data.mkdir()
        procs={};outputs=[]
        def start(n):
            out=open(root/f'node{n}-{len(outputs)}.log','w+')
            outputs.append(out)
            cert,private=node[n]
            procs[n]=subprocess.Popen([binary,'--elect-tls-auth-wal',str(cfgs[n-1]),
              str(data),str(keyring),str(cert),str(private),str(ca)],
              stdout=out,stderr=subprocess.STDOUT)
        def stop(n,hard=False):
            p=procs.get(n)
            if not p or p.poll() is not None:return
            if hard:p.kill()
            else:p.send_signal(signal.SIGTERM)
            try:p.wait(timeout=5)
            except subprocess.TimeoutExpired:p.kill();p.wait(timeout=3)
        def status(n,certfile=client,keyfile=client_key,ca_file=ca):
            return subprocess.run([binary,'--status-tls',str(cfgs[0]),str(n),
              str(certfile),str(keyfile),str(ca_file)],capture_output=True,text=True,timeout=4)
        def elected():
            state={n:status(n) for n in (1,2,3)}
            if not all(r.returncode==0 for r in state.values()):return None
            leaders=[n for n,r in state.items() if 'role=leader' in r.stdout]
            return leaders[0] if len(leaders)==1 else None
        def write(request,command,certfile=client,keyfile=client_key,ca_file=ca,budget=9000):
            return subprocess.run([binary,'--write-tls-auth-auto',str(cfgs[0]),
              str(budget),'71001',str(request),str(secret),str(certfile),
              str(keyfile),str(ca_file),command],capture_output=True,text=True,timeout=budget/1000+12)
        try:
            start(1);time.sleep(.16);start(2);time.sleep(.16);start(3)
            leader=support.wait_for(elected,20)
            assert leader in (1,2,3)
            print('PASS TLS: three nodes establish mutual-TLS Raft elections')
            first=write(1,'SET secured yes')
            assert first.returncode==0 and 'COMMITTED' in first.stdout,(first.stdout,first.stderr)
            duplicate=write(1,'SET secured yes')
            assert duplicate.returncode==0 and 'COMMITTED' in duplicate.stdout,(duplicate.stdout,duplicate.stderr)
            print('PASS TLS: encrypted HMAC-authenticated write and idempotent same-ID retry')
            # A different CA cannot impersonate the server or a client.
            bad_ca=status(leader,ca_file=wrong_ca)
            assert bad_ca.returncode!=0,(bad_ca.stdout,bad_ca.stderr)
            wrong_identity=status(leader,certfile=wrong_client,keyfile=wrong_client_key)
            assert wrong_identity.returncode!=0,(wrong_identity.stdout,wrong_identity.stderr)
            same_ca_outsider=status(leader,certfile=outsider,keyfile=outsider_key)
            assert same_ca_outsider.returncode!=0,(same_ca_outsider.stdout,same_ca_outsider.stderr)
            print('PASS TLS: wrong trust root, untrusted and unknown-SAN client certificates rejected')
            # Legacy plaintext protocol cannot reach TLS-only nodes.
            try:
                response=support.try_rpc(ports[leader-1],5,support.STATUS)
            except (OSError,AssertionError):response=None
            assert response is None,response
            print('PASS TLS: plaintext Raft/status frames cannot bypass TLS')
            before=write(2,'SET tamper no',certfile=wrong_client,keyfile=wrong_client_key,budget=850)
            assert before.returncode!=0,(before.stdout,before.stderr)
            print('PASS TLS: authenticated application write rejects wrong client certificate')
            # Even a trusted node certificate may not claim a DIFFERENT
            # configured sender ID inside an RPC. This probes the identity
            # binding, rather than merely TLS chain validation.
            target=1
            before_term=int(re.search(r'term=(\d+)',status(target).stdout).group(1))
            tls=ssl.create_default_context(cafile=str(ca))
            tls.load_cert_chain(str(node[3][0]),str(node[3][1]))
            raw=socket.create_connection(('127.0.0.1',ports[target-1]),timeout=2)
            with tls.wrap_socket(raw,server_hostname='raft-node-1') as channel:
                channel.settimeout(2)
                # RequestVote candidate=2, authenticated certificate=node-3.
                payload=struct.pack('!QIQQ',before_term+100,2,0,0)
                channel.sendall(struct.pack('!IBBHI',0x52414654,1,3,0,len(payload))+payload)
                try: reply=channel.recv(64)
                except (OSError,ssl.SSLError): reply=b''
                assert not reply,reply
            after_term=int(re.search(r'term=(\d+)',status(target).stdout).group(1))
            assert after_term==before_term,(before_term,after_term)
            print('PASS TLS: trusted peer cannot claim a different Raft candidate ID')
            # Starting a node with another member's valid certificate must
            # fail before it binds to the configured listener address.
            stop(2,hard=True)
            impostor=subprocess.run([binary,'--elect-tls-auth-wal',str(cfgs[1]),
              str(root/'impostor-data'),str(keyring),str(node[3][0]),
              str(node[3][1]),str(ca)],capture_output=True,text=True,timeout=4)
            assert impostor.returncode!=0 and 'SAN' in impostor.stderr,impostor
            print('PASS TLS: a node cannot start under another configured node identity')
            for n in (1,3):stop(n,hard=True)
            start(1);time.sleep(.16);start(2);time.sleep(.16);start(3)
            restored=support.wait_for(elected,20)
            assert restored in (1,2,3)
            again=write(1,'SET secured yes')
            if again.returncode!=0:
                barrier=write(2,'SET recovery yes')
                assert barrier.returncode==0,(barrier.stdout,barrier.stderr)
                again=write(1,'SET secured yes')
            assert again.returncode==0,(again.stdout,again.stderr)
            print('PASS TLS: snapshot-WAL crash recovery preserves authenticated retries')
        finally:
            for n in (1,2,3):stop(n,True)
            for out in outputs:out.close()
            if os.getenv('RAFT_TLS_DEBUG'):
                for log in root.glob('node*.log'):
                    print(f'---- {log.name} ----\n{log.read_text()[-5000:]}')

if __name__=='__main__':
    main(sys.argv[1] if len(sys.argv)>1 else 'build/raft-engine')
