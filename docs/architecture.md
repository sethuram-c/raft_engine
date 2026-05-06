# Architecture notes

This document describes the **current** code, rather than walking through every development phase. The implementation targets exactly three statically configured voters. The test matrix is broader than the supported deployment surface.

## Server and data ownership

`src/main.c` parses the CLI, loads a node configuration and selects the server or diagnostic mode. `src/raft_tcp.c` owns the listener, bounded worker pool, shared node state and the election/replication control loops. Worker access to mutable Raft state is synchronized. Connections and frames are bounded; a slow client must not create an unbounded thread-per-connection server.

The node stores its term, vote, role, log, commit index, last-applied index and election timer. Log entries own their command bytes. Requests received over the network are decoded into validated, bounded inputs before they are passed to consensus code.

## Consensus write path

```text
client request
    |
    v
leader admission / request-ID check
    |
    v
append to leader log ----> AppendEntries to followers
    |                            |
    |                    nextIndex / matchIndex
    +------------ majority acknowledgement
                                 |
                         current-term commit rule
                                 |
                   advance commitIndex and apply
                                 |
                        SET/DEL key-value map
```

RequestVote checks the candidate's term and last-log metadata before granting a vote. Election timers use a monotonic clock and randomized deadlines. Leaders issue AppendEntries heartbeats so followers do not start unnecessary elections.

For each follower, the leader maintains `nextIndex` and `matchIndex`. A rejected AppendEntries causes the leader to backtrack and retry. Conflict repair cannot overwrite an entry that is already committed. A leader may only advance its commit index directly based on a quorum-backed entry from its **current** term; the commit implicitly covers earlier entries.

After commitment, entries apply in index order. `lastApplied` cannot exceed `commitIndex`, and `commitIndex` cannot exceed the log's last index. The key-value engine supports `SET` and `DEL`, including binary-safe values. Unsupported command bytes are deterministic no-ops rather than process crashes.

## Retries and client sessions

A retryable request carries `(client_id, request_id)` and the command. The identity travels in the replicated log, is checked during admission, and is retained in state-machine snapshots. Repeating the latest request with the same payload can return its original result without another logical application. Reusing an ID with a different command is rejected.

The current session table is bounded and expects sequential request IDs from each client. It does not retain arbitrary historical request results. An uncertain timeout is not a failed write; callers must preserve the same identity when retrying. `--write-auto` probes the configured nodes for a leader and uses bounded retries.

## Persistence and snapshots

The repository retains multiple opt-in persistence modes because they were built and tested separately. Do not treat the formats as interchangeable.

- `raft_storage.c` implements full-state atomic persistence.
- `raft_wal.c` provides checksum-protected journal records and compacting checkpoints.
- `raft_snapshot.c` serializes applied key-value state and the included Raft index/term.
- `raft_install.c` transfers snapshot chunks over the network when a follower is too far behind for ordinary log replication.
- `raft_snapwal.c` coordinates snapshot state, log boundary, retained entries and Raft metadata in a recoverable generation.

A compacted log uses absolute indices. Its snapshot boundary represents the prefix no longer retained as individual entries. After restoring a snapshot, only the committed suffix beyond the boundary is applied.

Persistence is performed before a successful consensus acknowledgement where required. The code uses checksums, temporary files, synchronization, rename and directory synchronization in relevant paths. These mechanisms reduce common crash windows; they are not evidence of complete power-failure correctness on arbitrary storage devices.

## Network protocol and transport

`raft_wire.c` implements framed messages with explicit lengths and a bounded payload. The code includes RequestVote, AppendEntries, InstallSnapshot, client-write and read-only status/diagnostic messages. Do not send C structure layouts directly onto the wire.

The secure mode adds OpenSSL TLS 1.3 with mutual certificate validation. Raft sender IDs are matched to authenticated certificate identities. HMAC-SHA-256 separately authenticates retryable client write contents. The test harness provisions temporary certificates; it is not a replacement for a certificate lifecycle service. Legacy plain-TCP modes exist for local experimentation and should not be exposed outside an isolated environment.

TLS contexts can be reloaded on `SIGHUP`. Existing connections retain their context reference while new connections use the replacement. Failed reloads keep the last valid context. Handshakes have bounded deadlines, and the server has bounded worker capacity.

## Operational surfaces

`raft_metrics.c` provides counters and synchronized gauges; `raft_health.c` separates a responding process from a leader that has had recent quorum contact. Readiness is an operational signal only: recent acknowledgements do not establish a lease or guarantee that the next write will commit. Diagnostic GET results are not linearizable reads.

`raft_fault.c` intercepts selected outbound RPCs in a test-only mode. `tools/raft_chaos.py` runs seeded failure schedules, `tools/raft_bench.py` measures process-to-process write latency and resource use, and `tools/raft_capstone.py` runs the integrated TLS/auth/WAL scenario. Benchmarks include CLI startup and TCP overhead; the isolated log append microbenchmark measures a different part of the system.

## Things I would change before production use

1. Add a linearizable read path and test it under leader churn.
2. Implement safe membership changes and rolling upgrade/disk-format migration.
3. Build a real certificate/key rotation and revocation process.
4. Test recovery under power loss, partial filesystem operations, and disk exhaustion.
5. Introduce stronger resource quotas, fuzzing and formal or model-based safety checking.
6. Add an operationally defined client session retention and eviction policy.

The main learning value here is the path from an append-only in-memory log to a working consensus service with real crashes, retries and observability—not a claim that the remaining problems are already solved.
