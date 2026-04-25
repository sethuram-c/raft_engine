#!/usr/bin/env python3
"""Phase 25: authenticated admission, tampering, safe retries, optional durable replay."""
from __future__ import annotations
import hashlib
import hmac
import importlib.util
import secrets
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

spec=importlib.util.spec_from_file_location('support',Path(__file__).with_name('test_progress_integration.py'))
support=importlib.util.module_from_spec(spec)
assert spec and spec.loader
spec.loader.exec_module(support)
RESPONSE=struct.Struct('!QQB')


def main(binary):
    binary=str(Path(binary).resolve())
    with tempfile.TemporaryDirectory(prefix='raft-phase25-') as temp:
        root=Path(temp)
        ports=support.ports_three()
        cfgs=[]
        for n in (1,2,3):
            cfg=root/f'node{n}.conf'
            cfg.write_text(f'node_id={n}\n'+''.join(
                f'peer={i},127.0.0.1,{port}\n' for i,port in enumerate(ports,1)))
            cfgs.append(cfg)
        first_key=secrets.token_bytes(32)
        second_key=secrets.token_bytes(32)
        keyring=root/'keys'
        keyring.write_text(f'71001 {first_key.hex()}\n71002 {second_key.hex()}\n')
        keyring.chmod(0o600)
        secret=root/'secret';secret.write_text(first_key.hex()+'\n');secret.chmod(0o600)
        wrong=root/'wrong';wrong.write_text(second_key.hex()+'\n');wrong.chmod(0o600)
        storage=root/'wal';storage.mkdir()
        procs={};streams=[]
        def start(n):
            output=open(root/f'node{n}-{len(streams)}.log','w+')
            streams.append(output)
            procs[n]=subprocess.Popen([binary,'--elect-auth-wal',str(cfgs[n-1]),
                              str(storage),str(keyring)],stdout=output,stderr=subprocess.STDOUT)
        def stop(n,hard=False):
            p=procs.get(n)
            if p and p.poll() is None:
                if hard:p.kill()
                else:p.send_signal(signal.SIGTERM)
                try:p.wait(timeout=4)
                except subprocess.TimeoutExpired:p.kill();p.wait(timeout=2)
        def status(n):return support.try_rpc(ports[n-1],5,support.STATUS)
        def log(n):return support.try_rpc(ports[n-1],9,support.LOG)
        def elected():
            states={n:status(n) for n in (1,2,3)}
            if any(s is None for s in states.values()):return None
            leaders=[n for n,s in states.items() if s[2]==2]
            terms={s[1] for s in states.values()}
            return leaders[0] if len(leaders)==1 and len(terms)==1 else None
        def auto(request,command,secret_file=secret,client=71001,budget=7000):
            return subprocess.run([binary,'--write-auth-auto',str(cfgs[0]),str(budget),
                 str(client),str(request),str(secret_file),command],
                 capture_output=True,text=True,timeout=budget/1000+8)
        def secure_body(client,request,cmd,key,timeout=450):
            body=struct.pack('!IQQ',timeout,client,request)+cmd
            mac=hmac.new(key,b'RAFT25-CLIENT-WRITE-V1'+body,hashlib.sha256).digest()
            return body+mac
        def raw(n,kind,body):
            try:return support.rpc(ports[n-1],kind,body)
            except (OSError,AssertionError):return None
        try:
            start(1);time.sleep(.15);start(2);time.sleep(.15);start(3)
            leader=support.wait_for(elected,15)
            first=auto(1,'SET secure yes')
            assert first.returncode==0 and 'status=COMMITTED' in first.stdout,first
            idx=log(leader)[0]
            assert idx>=1
            print('PASS: authenticated write automatically locates leader and commits with quorum')
            duplicate=auto(1,'SET secure yes')
            assert duplicate.returncode==0 and 'COMMITTED' in duplicate.stdout,duplicate
            assert log(leader)[0]==idx
            print('PASS: signed retry uses the previous replicated request ID')
            before=log(leader)[0]
            bad_key=auto(2,'SET intruder denied',wrong,budget=800)
            assert bad_key.returncode==4 and log(leader)[0]==before,bad_key
            identity=auto(2,'SET intruder denied',secret,client=71002,budget=800)
            assert identity.returncode==4 and log(leader)[0]==before,identity
            print('PASS: wrong client secret and substituted client identity cannot write')
            request=secure_body(71001,2,b'SET signed valid',first_key)
            for mutate in (0,4,12,20,len(request)-1):
                tampered=bytearray(request);tampered[mutate]^=1
                response=raw(leader,25,bytes(tampered))
                assert response is not None and response[0]==26 and RESPONSE.unpack(response[1])[2]==3,response
                assert log(leader)[0]==before
            print('PASS: HMAC covers timeout, client ID, request ID, command and tag')
            legacy=raw(leader,23,struct.pack('!IQQ',450,71001,2)+b'SET bypass no')
            basic=raw(leader,21,struct.pack('!I',450)+b'SET bypass2 no')
            diagnostic=raw(leader,11,b'SET bypass3 no')
            compact=raw(leader,19,b'')
            assert legacy and legacy[0]==24 and RESPONSE.unpack(legacy[1])[2]==3
            assert all(x is None for x in (basic,diagnostic,compact))
            assert log(leader)[0]==before
            print('PASS: legacy unauthenticated writes and diagnostic mutation bypasses disabled')
            unknown=secure_body(72999,2,b'SET unknown no',first_key)
            out=raw(leader,25,unknown)
            assert out and out[0]==26 and RESPONSE.unpack(out[1])[2]==3
            conflict=auto(1,'SET secure altered',budget=1000)
            assert conflict.returncode==4 and log(leader)[0]==before
            print('PASS: unknown identities and conflicting request reuse rejected')
            second=auto(2,'SET persist yes')
            assert second.returncode==0 and 'COMMITTED' in second.stdout,second
            support.wait_for(lambda:all((s:=log(n)) and s[2]>=idx+1 for n in (1,2,3)),12)
            for n in (1,2,3):stop(n,hard=True)
            start(1);time.sleep(.15);start(2);time.sleep(.15);start(3)
            new_leader=support.wait_for(elected,15)
            duplicate=auto(2,'SET persist yes',budget=8500)
            # If the leader's current-term commit index has not advanced, a
            # current-term barrier can safely commit earlier entries.
            if duplicate.returncode!=0:
                barrier=auto(3,'SET barrier yes',budget=8500)
                assert barrier.returncode==0,barrier
                duplicate=auto(2,'SET persist yes',budget=8500)
            assert duplicate.returncode==0 and 'COMMITTED' in duplicate.stdout,duplicate
            assert log(new_leader)[0] in (idx+1,idx+2)
            print('PASS: signed request identity survives snapshot-WAL SIGKILL and leader recovery')
            secret.chmod(0o644)
            fail=auto(4,'SET private no',budget=800)
            assert fail.returncode==4
            print('PASS: insecure credential permissions fail closed')
        except Exception:
            for stream in streams:
                stream.flush()
                print(f'--- {stream.name} ---\n{Path(stream.name).read_text()[-2400:]}',file=sys.stderr)
            raise
        finally:
            for n in (1,2,3):stop(n)
            for stream in streams:stream.close()

if __name__=='__main__':main(sys.argv[1])
