#!/usr/bin/env python3
"""Phase 24: discovery, no-quorum uncertainty, safe same-ID retry, failover.

Ports and all storage are private to this test; no prior configuration needed.
"""
from __future__ import annotations
import importlib.util
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    'support', Path(__file__).with_name('test_progress_integration.py'))
assert spec and spec.loader
support = importlib.util.module_from_spec(spec)
spec.loader.exec_module(support)


def main(binary: str) -> None:
    binary = str(Path(binary).resolve())
    with tempfile.TemporaryDirectory(prefix='raft-phase24-') as tmp:
        base = Path(tmp)
        ports = support.ports_three()
        configs=[]
        for n in (1,2,3):
            cfg=base/f'node{n}.conf'
            cfg.write_text(f'node_id={n}\n'+''.join(
                f'peer={i},127.0.0.1,{port}\n' for i,port in enumerate(ports,1)))
            configs.append(cfg)
        procs={}; streams=[]

        def start(n):
            stream=open(base/f'node{n}-{len(streams)}.log','w+')
            streams.append(stream)
            procs[n]=subprocess.Popen([binary,'--elect',str(configs[n-1])],
                                    stdout=stream,stderr=subprocess.STDOUT)

        def stop(n):
            p=procs.get(n)
            if p is not None and p.poll() is None:
                p.send_signal(signal.SIGTERM)
                try:p.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    p.kill();p.wait(timeout=3)

        def status(n):
            return support.try_rpc(ports[n-1],5,support.STATUS)

        def log(n):
            return support.try_rpc(ports[n-1],9,support.LOG)

        def leader(nodes):
            states={n:status(n) for n in nodes}
            if any(s is None for s in states.values()):return None
            ids=[n for n,s in states.items() if s[2]==2]
            terms={s[1] for s in states.values()}
            return ids[0] if len(ids)==1 and len(terms)==1 else None

        def auto(client,request,cmd,budget=5000,config=None):
            return subprocess.run([binary,'--write-auto',
                str(config or configs[0]),str(budget),str(client),str(request),cmd],
                capture_output=True,text=True,timeout=budget/1000+7)

        try:
            start(1);time.sleep(.18);start(2);time.sleep(.18);start(3)
            elected=support.wait_for(lambda:leader((1,2,3)),15)
            first=auto(90901,1,'SET routing yes',8000,configs[elected-1])
            assert first.returncode==0 and 'status=COMMITTED' in first.stdout,first
            first_log=log(elected)
            assert first_log and first_log[0]>=1,first_log
            first_index=first_log[0]
            print('PASS: autodiscovery succeeds regardless of the client configuration node ID')
            repeated=auto(90901,1,'SET routing yes',6000)
            assert repeated.returncode==0 and 'status=COMMITTED' in repeated.stdout,repeated
            assert log(elected)[0]==first_index
            print('PASS: repeated automatic request returns original commit without a new log entry')
            conflict=auto(90901,1,'SET routing no',3500)
            assert conflict.returncode==4 and 'status=REJECTED' in conflict.stdout,conflict
            assert log(elected)[0]==first_index
            print('PASS: conflicting same-ID command is rejected rather than retried indefinitely')

            others=[n for n in (1,2,3) if n!=elected]
            stop(others[0]);stop(others[1])
            start_time=time.monotonic()
            uncertain=auto(90901,2,'SET needs quorum',1800)
            elapsed=time.monotonic()-start_time
            assert uncertain.returncode==3 and 'status=UNKNOWN' in uncertain.stdout,uncertain
            assert 1.2<elapsed<4.5,elapsed
            pending=log(elected)
            assert pending and pending[0]==first_index+1 and pending[2]==first_log[2],pending
            print('PASS: bounded overall deadline returns indeterminate without a quorum')
            start(others[0]);time.sleep(.2)
            support.wait_for(lambda:(x:=log(elected)) is not None and
                             x[2]>=pending[0],15)
            retry=auto(90901,2,'SET needs quorum',6500)
            assert retry.returncode==0 and 'status=COMMITTED' in retry.stdout,retry
            assert log(elected)[0]==pending[0]
            print('PASS: same ID safely resolves a timed-out operation after quorum returns')

            stop(elected)
            # Bring the third node online if needed; use the existing survivor pair.
            start(others[1]);time.sleep(.2)
            next_leader=support.wait_for(lambda:leader(tuple(others)),18)
            # Raft can only advance a new leader's commit index using a
            # current-term entry; an old-term entry alone is insufficient.
            barrier=auto(90902,1,'SET new_term barrier',9000,configs[elected-1])
            assert barrier.returncode==0 and 'status=COMMITTED' in barrier.stdout,barrier
            final=auto(90901,2,'SET needs quorum',9000,configs[elected-1])
            assert final.returncode==0 and 'status=COMMITTED' in final.stdout,final
            assert log(next_leader)[0]==pending[0]+1
            print('PASS: client rediscovers a new leader and keeps the original request ID')
        except Exception:
            for stream in streams:
                stream.flush()
                print(f'--- {stream.name} ---',file=sys.stderr)
                print(Path(stream.name).read_text()[-3000:],file=sys.stderr)
            raise
        finally:
            for n in (1,2,3):stop(n)
            for stream in streams:stream.close()

if __name__=='__main__':main(sys.argv[1])
