#!/usr/bin/env python3
"""Phase 21: durable snapshot installation and WAL recovery across SIGKILL."""
from __future__ import annotations
import importlib.util
import signal
import struct
import subprocess
import sys
import time
import tempfile
from pathlib import Path

spec=importlib.util.spec_from_file_location(
    'support',Path(__file__).with_name('test_progress_integration.py'))
assert spec and spec.loader
support=importlib.util.module_from_spec(spec)
spec.loader.exec_module(support)
APPLY=struct.Struct('!QQIQ')
READ=struct.Struct('!BQH')


def main(binary):
    binary=str(Path(binary).resolve())
    with tempfile.TemporaryDirectory(prefix='raft-phase21-') as directory:
        base=Path(directory)
        ports=support.ports_three()
        cfgs=[]
        for node in (1,2,3):
            conf=base/f'{node}.conf'
            conf.write_text(f'node_id={node}\n'+''.join(
                f'peer={i},127.0.0.1,{p}\n' for i,p in enumerate(ports,1)))
            cfgs.append(conf)
        data=base/'data'
        data.mkdir()
        procs={}; streams=[]
        def start(n):
            f=open(base/f'{n}-run-{len(streams)}.log','w+')
            streams.append(f)
            procs[n]=subprocess.Popen([binary,'--elect-snapshot-wal',str(cfgs[n-1]),str(data)],
                                     stdout=f,stderr=subprocess.STDOUT)
        def stop(n):
            p=procs.get(n)
            if p and p.poll() is None:
                p.send_signal(signal.SIGTERM)
                try:p.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    p.kill();p.wait(timeout=3)
        def status(n): return support.try_rpc(ports[n-1],5,support.STATUS)
        def log(n):return support.try_rpc(ports[n-1],9,support.LOG)
        def apply(n):return support.try_rpc(ports[n-1],13,APPLY)
        def leader():
            sts={n:status(n) for n in (1,2)}
            if not all(sts.values()):return None
            ls=[n for n,s in sts.items() if s[2]==2]
            if len(ls)!=1 or sts[1][1]!=sts[2][1]:return None
            return ls[0],sts[1][1]
        def propose(n,command):
            typ,body=support.rpc(ports[n-1],11,command)
            assert typ==12 and len(body)==support.PROPOSE_REPLY.size
            return support.PROPOSE_REPLY.unpack(body)
        def read(n,key):
            typ,body=support.rpc(ports[n-1],15,key.encode())
            assert typ==16 and len(body)>=READ.size
            found,index,vl=READ.unpack(body[:READ.size])
            assert len(body)==READ.size+vl
            return found,index,body[READ.size:]
        def compact(n):
            return subprocess.run([binary,'--snapshot-compact-demo',
                str(cfgs[2]),str(n)],capture_output=True,text=True,timeout=5)
        try:
            start(1)
            # A small stagger avoids synchronized split-vote retries on
            # heavily loaded CI hosts with slow journal fsync latency.
            time.sleep(0.25)
            start(2) # two nodes form a quorum while node 3 is absent
            elected,term=support.wait_for(leader,18)
            denied=compact(3)  # the peer isn't running; errors must be bounded
            assert denied.returncode!=0
            print('PASS: unavailable snapshot target is bounded and rejected')
            # Persist enough distinct values to exceed the 4096-byte frame limit.
            for i in range(7):
                content=(f'V{i}'.encode() * 470)[:940]
                command=f'SET key{i} '.encode()+content
                assert propose(elected,command)==(term,i+1,1)
                support.wait_for(lambda:all((a:=apply(n)) is not None and
                                     a[:2]==(i+1,i+1) for n in (1,2)),12)
            print('PASS: two-voter majority commits seven binary-safe values')
            # Leader-only operation should not succeed on the second node.
            other=next(n for n in (1,2) if n!=elected)
            declined=subprocess.run([binary,'--snapshot-compact-demo',
                str(cfgs[2]),str(other)],capture_output=True,text=True,timeout=4)
            assert declined.returncode!=0 and 'accepted=0' in declined.stdout,declined
            out=compact(elected)
            assert out.returncode==0 and 'accepted=1' in out.stdout,out
            # CLI prints the compacted (index, term) boundary and acceptance.
            assert 'boundary=7/' in out.stdout,out
            print('PASS: elected leader compacts at lastApplied=7; follower rejects admin action')
            assert propose(elected,b'SET tail survived')==(term,8,1)
            support.wait_for(lambda:all((a:=apply(n)) is not None and
                                     a[:2]==(8,8) for n in (1,2)),12)
            start(3)
            support.wait_for(lambda:(a:=apply(3)) is not None and a[:2]==(8,8),12)
            assert read(3,'key5')[0]==1
            assert read(3,'tail')==(1,8,b'survived')
            support.wait_for(lambda: log(3) and log(3)[0]==8,6)
            # A completed multi-frame install is visible in the follower log.
            logs=[f.name for f in streams]
            follower_log=Path(logs[-1]).read_text()
            assert 'Snapshot installed:' in follower_log,follower_log
            assert 'boundary=7/' in follower_log,follower_log
            print('PASS: late third node installs multi-frame snapshot and catches up via AppendEntries')
            # Continuing replication after catching up demonstrates retained index continuity.
            elected2,term2=support.wait_for(leader,8)
            assert propose(elected2,b'SET follow yes')==(term2,9,1)
            support.wait_for(lambda:all((a:=apply(n)) is not None and
                                     a[:2]==(9,9) for n in (1,2,3)),12)
            assert all(read(n,'follow')==(1,9,b'yes') for n in (1,2,3))
            print('PASS: fresh log replication and commitment continue beyond compacted boundary')
            # Recover all three persisted identities after a hard process kill.
            # No single node may acknowledge an uncommitted suffix as applied.
            for n in (1,2,3):
                p=procs[n]
                p.kill();p.wait(timeout=4)
            for n in (1,2,3):start(n)
            support.wait_for(lambda:all((a:=apply(n)) is not None and
                                     a[:2]==(9,9) for n in (1,2,3)),15)
            assert all(read(n,'key5')[0]==1 and
                       read(n,'tail')==(1,9,b'survived') and
                       read(n,'follow')==(1,9,b'yes') for n in (1,2,3))
            assert all((data/f'node-{n}.snapwal').exists() for n in (1,2,3))
            print('PASS: SIGKILL/restart restores snapshot plus committed WAL suffix on all nodes')
            # Orphan pre-rename files must not replace the authoritative WAL.
            (data/'node-3.snapwal.tmp').write_bytes(b'UNCOMMITTED-PARTIAL-CHECKPOINT')
            stop(3);start(3)
            support.wait_for(lambda:(a:=apply(3)) is not None and a[:2]==(9,9),12)
            assert read(3,'key5')[0]==1
            assert not (data/'node-3.snapwal.tmp').exists()
            print('PASS: orphan temporary checkpoint ignored during recovery')

            # Refuse an InstallSnapshot from a nonmember (type 17 with leader ID 99).
            malformed=struct.pack('!QIQQIIBBH',term2+1,99,7,term,0,52,0,0,1)+b'X'
            assert len(malformed)==41
            previous_status=status(3)
            assert previous_status is not None
            try:
                support.rpc(ports[2],17,malformed)
            except (OSError,AssertionError):pass
            else:raise AssertionError('accepted nonmember snapshot')
            assert status(3) == previous_status
            print('PASS: nonmember InstallSnapshot request cannot change term or role')
            # A complete corrupted record cannot be treated as a recoverable
            # torn tail. Stop node 3, corrupt record payload, and refuse boot.
            stop(3)
            path=data/'node-3.snapwal'
            with path.open('r+b') as f:
                f.seek(24)
                b=f.read(1)
                assert b
                f.seek(24);f.write(bytes([b[0]^0x20]))
            corrupt=subprocess.run([binary,'--elect-snapshot-wal',str(cfgs[2]),
                                    str(data)],capture_output=True,text=True,timeout=5)
            assert corrupt.returncode!=0,corrupt
            assert 'CRC' in corrupt.stderr or 'corrupt' in corrupt.stderr.lower(),corrupt.stderr
            print('PASS: corrupted complete snapshot/WAL record blocks startup')

        except Exception:
            for f in streams:
                f.flush()
                print(f'--- {f.name} ---',file=sys.stderr)
                print(Path(f.name).read_text()[-6500:],file=sys.stderr)
            raise
        finally:
            for n in list(procs):stop(n)
            for f in streams:f.close()


if __name__=='__main__':main(sys.argv[1])
