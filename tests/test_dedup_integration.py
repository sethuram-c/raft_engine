#!/usr/bin/env python3
"""Phase 23: retries, leader rotation, snapshot compaction and crash recovery.

Uses snapshot-aware WAL so request identity must survive log-prefix compaction.
"""
from __future__ import annotations
import importlib.util
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

spec=importlib.util.spec_from_file_location('support',
    Path(__file__).with_name('test_progress_integration.py'))
assert spec and spec.loader
support=importlib.util.module_from_spec(spec)
spec.loader.exec_module(support)


def main(binary):
    binary=str(Path(binary).resolve())
    with tempfile.TemporaryDirectory(prefix='raft-phase23-') as tmp:
        base=Path(tmp); data=base/'data'; data.mkdir()
        ports=support.ports_three(); configs=[]
        for n in (1,2,3):
            cfg=base/f'{n}.conf'
            cfg.write_text(f'node_id={n}\n'+''.join(
                f'peer={i},127.0.0.1,{port}\n' for i,port in enumerate(ports,1)))
            configs.append(cfg)
        procs={}; streams=[]

        def start(n):
            f=open(base/f'node{n}-{len(streams)}.log','w+')
            streams.append(f)
            procs[n]=subprocess.Popen([binary,'--elect-snapshot-wal',
                str(configs[n-1]),str(data)],stdout=f,stderr=subprocess.STDOUT)

        def stop(n,hard=False):
            p=procs.get(n)
            if p is not None and p.poll() is None:
                p.send_signal(signal.SIGKILL if hard else signal.SIGTERM)
                try: p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    p.kill();p.wait(timeout=3)

        def status(n):
            return support.try_rpc(ports[n-1],5,support.STATUS)

        def log(n):
            return support.try_rpc(ports[n-1],9,support.LOG)

        def leader(nodes=(1,2,3)):
            states={n:status(n) for n in nodes}
            if any(v is None for v in states.values()): return None
            ids=[n for n,s in states.items() if s[2]==2]
            terms={s[1] for s in states.values()}
            return ids[0] if len(ids)==1 and len(terms)==1 else None

        def write(n,client,request,command,timeout=2300):
            # choose a different configuration so self-target prevention applies.
            config=configs[n % 3]
            return subprocess.run([binary,'--write-id',str(config),str(n),
                str(timeout),str(client),str(request),command],
                capture_output=True,text=True,timeout=9)

        def check(result,code,text):
            assert result.returncode==code and text in result.stdout, result

        try:
            start(1);time.sleep(.18);start(2);time.sleep(.18);start(3)
            elected=support.wait_for(leader,16)
            follower=[n for n in (1,2,3) if n!=elected]
            first=write(elected,77,1,'SET token stable')
            check(first,0,'status=COMMITTED')
            first_log=log(elected);assert first_log and first_log[0]>=1
            original_index=first_log[0]
            check(write(elected,77,1,'SET token stable'),0,'status=COMMITTED')
            assert log(elected)[0]==original_index
            print('PASS: retry of confirmed request returns same commitment without a second log entry')
            check(write(elected,77,1,'SET token altered'),4,'status=REJECTED')
            assert log(elected)[0]==original_index
            print('PASS: same request ID with different command is rejected, not applied')
            check(write(elected,77,2,'SET token next'),0,'status=COMMITTED')
            check(write(elected,77,1,'SET token stable'),4,'status=REJECTED')
            assert log(elected)[0]==original_index+1
            print('PASS: increasing request numbers accepted; stale numbers rejected')
            check(write(follower[0],77,2,'SET token next'),2,'status=NOT_LEADER')
            print('PASS: follower rejects retryable writes without appending')

            # Force a new leader; peers must recognize the committed identity
            # after an election, not only in the original leader's RAM.
            support.wait_for(lambda: all((x:=log(n)) and x[2]>=original_index+1
                                         for n in follower),12)
            stop(elected,hard=True)
            replacement=support.wait_for(lambda: leader(tuple(follower)),16)
            previous=log(replacement)[0]
            check(write(replacement,77,2,'SET token next'),0,'status=COMMITTED')
            assert log(replacement)[0]==previous
            print('PASS: a newly elected leader recognizes an earlier committed request')

            # Persist the dedup session in a compacted snapshot, then reload it.
            compact=subprocess.run([binary,'--snapshot-compact-demo',
                str(configs[replacement % 3]),str(replacement)],
                capture_output=True,text=True,timeout=8)
            assert compact.returncode==0,compact
            stop(follower[0],hard=True);stop(follower[1],hard=True)
            time.sleep(.2)
            start(follower[0]);time.sleep(.25);start(follower[1]);
            resumed=support.wait_for(lambda: leader(tuple(follower)),20)
            before=log(resumed)[0]
            check(write(resumed,77,2,'SET token next'),0,'status=COMMITTED')
            assert log(resumed)[0]==before
            print('PASS: compacted request table survives snapshot-WAL SIGKILL/restart')

            # Timeout is uncertain: retry the SAME identity, never a new one.
            other=next(n for n in follower if n!=resumed)
            stop(other,hard=True)
            uncertain=write(resumed,77,3,'SET token eventually',timeout=350)
            check(uncertain,3,'status=TIMEOUT')
            pending_index=log(resumed)[0]
            assert pending_index==before+1
            pending_retry=write(resumed,77,3,'SET token eventually',timeout=350)
            check(pending_retry,3,'status=TIMEOUT')
            assert log(resumed)[0]==pending_index
            check(write(resumed,77,3,'SET token conflict',timeout=350),4,'status=REJECTED')
            check(write(resumed,77,4,'SET token premature',timeout=350),4,'status=REJECTED')
            assert log(resumed)[0]==pending_index
            print('PASS: repeated timeout, conflicting payload and overlapping ID do not append')
            start(other)
            support.wait_for(lambda: (x:=log(resumed)) and x[2]>=pending_index,18)
            check(write(resumed,77,3,'SET token eventually'),0,'status=COMMITTED')
            assert log(resumed)[0]==pending_index
            print('PASS: timeout followed by a same-ID retry does not duplicate the write')

            # Last member rejoins after leader discards prefix; installing a
            # v2 snapshot must restore its dedup session table too.
            compact=subprocess.run([binary,'--snapshot-compact-demo',
                str(configs[resumed % 3]),str(resumed)],
                capture_output=True,text=True,timeout=8)
            assert compact.returncode==0,compact
            start(elected)
            support.wait_for(lambda: (x:=log(elected)) is not None and
                             x[0]>=pending_index and x[2]>=pending_index,20)
            support.wait_for(lambda: any('Snapshot installed:' in
                             Path(f.name).read_text() for f in streams if
                             f.name.endswith(f'node{elected}-{len(streams)-1}.log')),
                             3)
            print('PASS: lagging restarted node installs a request-aware v2 snapshot')
        except Exception:
            for f in streams:
                f.flush()
                print(f'--- {f.name} ---',file=sys.stderr)
                print(Path(f.name).read_text()[-3600:],file=sys.stderr)
            raise
        finally:
            for n in (1,2,3): stop(n)
            for f in streams: f.close()

if __name__=='__main__':main(sys.argv[1])
