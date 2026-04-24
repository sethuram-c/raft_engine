# Development progress

## Phase 1 / 35 - Fedora setup and GNU Make

**Implemented:** project skeleton, C11 executable, Git-ready layout, smoke tests.

## Phase 2 / 35 - Core Raft types and in-memory log

**Implemented:** node lifecycle, state/term/index types, binary-safe dynamic log,
local invariants, nine unit tests.

## Phase 3 / 35 - Static three-node configuration

**Implemented:** strict fixed-three-node parser, IPv4 endpoint validation,
example configuration files, CLI output and negative tests.

## Phase 4 / 35 - TCP server and peer connections

**Time budget:** ~2 hours
**Planned milestone:** January 4, 2027 (record actual commit time)

### Deliverable

A runnable TCP server that listens on the local configured IPv4:port, accepts
connections, logs them, cleans up file descriptors and shuts down gracefully;
plus a bounded-time peer connection probe and real loopback integration tests.

### Acceptance criteria

- [x] Listener binds configured local node endpoint
- [x] Listener accepts and closes real TCP sockets
- [x] `--probe` selects a configured peer ID and connects to it
- [x] `--probe` has bounded connection timeout and useful errors
- [x] Listener handles `SIGINT` and `SIGTERM` without unsafe signal work
- [x] Second listener on occupied endpoint is rejected
- [x] Invalid peer IDs and connection-count arguments are rejected
- [x] `make test` includes prior and new tests
- [x] Network tests use temporary ports (do not assume 7001-7003 are free)
- [ ] Run the complete acceptance tests on your own Fedora system
- [ ] Commit work after manual review

### Verify

```sh
make clean && make && make test
make check-config
./build/raft-engine --serve config/node1.conf
# In another terminal:
./build/raft-engine --probe config/node2.conf 1
```

### Commit message

```text
feat: add TCP listeners and peer connectivity probes
```

**Scope limit:** The accept loop closes each accepted descriptor immediately.
Raft RPCs and framed messages belong to later phases. No consensus yet.

## Phase 5 / 35 - TCP message framing and serialization

**Time budget:** ~2 hours  
**Planned milestone:** January 5, 2027 (record actual commit time)

### Deliverable

A portable-on-the-wire, versioned, length-prefixed PING/PONG frame protocol
used by two independent Linux processes. Binary payloads are length-tracked,
validated and bounded to 4096 bytes. The listener handles one frame per
connection, echoes PING as PONG and closes the socket.

### Acceptance criteria

- [x] Explicit network-order 12-byte header with magic/version/type/length
- [x] Reject unknown types, reserved flags, bad versions and oversize lengths
- [x] send/recv exact-byte loops support partial TCP I/O
- [x] Clean EOF distinguished from truncated header or payload
- [x] Binary-safe payloads including zero bytes and zero-length frames
- [x] PING/PONG exchange across real independent processes
- [x] Previous Phase 1-4 tests still pass
- [x] Malformed frame integration tests use loopback and temporary ports
- [ ] Run all acceptance tests on your personal Fedora machine
- [ ] Review and commit with an actual completion timestamp

### Verify

```sh
make clean && make && make test
# terminal 1: ./build/raft-engine --serve config/node1.conf
# terminal 2: ./build/raft-engine --ping config/node2.conf 1 hello-raft
```

### Commit message

```text
feat: add bounded TCP framing and binary-safe ping pong messages
```

**Scope limit:** Not real Raft RPC or a concurrent server. Threaded request
handling is planned for Phase 6. Personal learning PDF stays outside Git.

## Phase 6 / 35 - Concurrent connection handling with POSIX threads

**Time budget:** ~2 hours  
**Planned milestone:** January 6, 2027 (use actual commit date)

### Deliverable

A bounded eight-worker POSIX threaded TCP server that can answer multiple
PINGs without head-of-line blocking behind one stalled client. Idle workers
are joined and reused; full capacity applies backpressure. SIGINT/SIGTERM
interrupts active sockets and waits for threads to finish.

### Acceptance criteria

- [x] `-pthread` compile/link support in GNU Make
- [x] Exactly eight maximum concurrent connection workers
- [x] Mutex protects socket lifetime and worker completion flags
- [x] Condition variable avoids spinning while full
- [x] Join completed workers before reusing slots
- [x] PING/PONG remains compatible with Phase 5 protocol
- [x] Parallel clients finish while another client stalls
- [x] A ninth connection waits for an available worker
- [x] SIGTERM gracefully interrupts waiting workers and joins them
- [x] All previous tests still pass
- [ ] Verify on Fedora hardware and review output
- [ ] Commit source files using real completion date

### Verify

```sh
make clean && make && make test
# terminal 1: ./build/raft-engine --serve config/node1.conf
# terminal 2: for i in $(seq 1 6); do ./build/raft-engine --ping config/node2.conf 1 "request-$i" & done; wait
```

### Commit message

```text
feat: handle concurrent TCP clients with bounded pthread workers
```

**Scope limit:** Real Raft RPCs and leader election are NOT implemented.
The separate Phase 6 study PDF must not enter Git.

## Phase 7 / 35 - Follower state and randomized election timers

**Time budget:** ~2 hours  
**Planned milestone:** January 7, 2027 (record actual commit time)

### Deliverable

A monotonic-clock-backed randomized election timeout module wired into the
Raft follower state, with an injectable logical timestamp for deterministic
unit tests and a `--timer-demo` CLI. The timer reports when an election would
be due, but it deliberately does not change the node to candidate or send RPCs.

### Acceptance criteria

- [x] Read real elapsed time with CLOCK_MONOTONIC
- [x] Independently seeded repeatable xorshift32 jitter within 350-700 ms
- [x] Timer reset, expiry and remaining-time APIs
- [x] Deadline inclusivity and uint64 overflow checks
- [x] Role-gated follower timeout query; role/term/vote invariants preserved
- [x] CLI demo with two checks at the exact deadline boundary and a reset
- [x] Timer tests for invalid inputs, randomness, clock and follower lifecycle
- [x] All earlier smoke, unit and network integration tests continue to pass
- [ ] Reproduce the tests on your own Fedora workstation
- [ ] Review and commit source with the actual completion timestamp

### Verify

```sh
make clean && make && make test
./build/raft-engine --timer-demo config/node1.conf
```

### Commit message

```text
feat: add randomized follower election timers with monotonic deadlines
```

**Scope limit:** This phase does not implement RequestVote, leader election,
heartbeat validation, shared-state locking, or timers on live network nodes.
The learning PDF stays outside the Git repo.

## Phase 8 / 35 - RequestVote RPC and follower vote rules

**Deliverable:** bounded, binary-safe RequestVote request/response RPC over
real TCP with shared synchronized in-memory term/vote state.

- [x] Add `raft_vote.h` and `raft_vote.c` with strict big-endian codec
- [x] Add RequestVote / response message types to existing wire frame
- [x] Implement Raft term, single-vote and candidate-log freshness checks
- [x] Reset election timeout only for granted votes
- [x] Serialize worker-thread vote decisions under mutex
- [x] Reject candidate IDs not in the configured cluster
- [x] Implement one-shot `--request-vote` CLI
- [x] Cover raw protocol, vote decisions, malformed messages and TCP races
- [x] Preserve earlier phase tests under normal GCC build

Acceptance criteria: `make`, `make test`, and raw TCP RequestVote integration
should succeed on Fedora Linux. Do not confuse a granted vote with a leader
election: candidacy, self-voting, majority counting and heartbeats are later.
Current term and votes are **not durable**; the server is not safe for real
Raft elections after process restarts. Private learning PDF stays outside Git.

## Phase 9 / 35 - Automatic leader election

**Deliverable:** real candidate timeouts, self-votes, RequestVote RPC fanout,
majority-backed leader transition, and read-only election status over TCP.

- [x] Add testable `raft_election_begin` and `raft_election_record` APIs
- [x] Count self-vote and only distinct peer replies in the current term
- [x] Step down when responses contain a higher term
- [x] Retry failed candidate elections after a randomized timeout
- [x] Run election worker separately from bounded TCP request workers
- [x] Preserve passive `--serve` behavior for previous-phase tests
- [x] Introduce `--elect` and `--status` commands
- [x] Test isolated node, two-node majority and joining third node
- [x] Keep handbook PDF outside the source repository
- [ ] Run on Fedora and commit after verifying locally

**Known limits:** leader heartbeats, leader stability, durable votes, crash
safety and log replication are not implemented in Phase 9. Status endpoints
are debug-only and unauthenticated. One elected leader is a momentary fact,
not a leadership lease or guaranteed stable leader.

**Commit:** `feat: implement automatic raft leader elections over TCP`


## Phase 10 / 35 - AppendEntries heartbeats and stable election

**Time budget:** ~2 hours. **Planned:** January 10, 2027.

**Deliverable:** three C processes automatically elect and maintain a single
leader under healthy localhost communication using empty AppendEntries RPCs.

- [x] Define a 36-byte AppendEntries (empty heartbeat) request and 9-byte reply
- [x] Add frame types 7 and 8 to the bounded RAFT wire protocol
- [x] Reject malformed and nonmember leader IDs before changing term or timer
- [x] Ignore stale terms without resetting the election timeout
- [x] On current/higher term, recognize the leader and reset election deadline
- [x] Reply to previous-log mismatch while still honoring valid leader contact
- [x] Step down on higher-term heartbeat responses
- [x] Send heartbeats without holding the mutex during socket I/O
- [x] Keep 350-700ms follower election timers and add ~90ms heartbeat interval
- [x] Test stable 3-node leadership, leader failure, 2-node reelection, and rejoin
- [x] Preserve all prior tests and run sanitizer checks
- [ ] Run on personal Fedora workstation, inspect code, and commit with actual time

**Verify:** `make clean && make && make test`, then run three `--elect` nodes
and query each with `--status`. `tests/test_heartbeat_integration.py` uses
nonfixed localhost ports. Phase 10 does not yet replicate log entries,
write durable votes or prove crash safety.

**Commit:** `feat: add AppendEntries heartbeats for stable raft leadership`


## Phase 11 / 35 - In-memory Raft log matching and conflict repair

**Time budget:** ~2 hours. **Planned date:** January 11, 2027.

**Deliverable:** a safe, unit-tested in-memory log reconciliation API that
preserves matching prefixes, rejects previous-log mismatches and committed
entry overwrites, deep-copies new commands and does not truncate on heartbeat.

- [x] Implement `raft_log_matches` for the `(index, term)` check
- [x] Implement `raft_log_last_term` with virtual `(0,0)` prefix
- [x] Add `raft_log_reconcile` with nondecreasing-term validation
- [x] Reject conflicts within committed entries
- [x] Validate allocation/count overflow and perform failure-atomic repair
- [x] Add Phase 11 tests and `--log-demo` CLI
- [x] Preserve Phase 1-10 behavior in the normal test suite
- [ ] Run and verify on personal Fedora workstation
- [ ] Commit reviewed source using actual completion timestamp

**Verify:** `make clean && make && make test && make demo` and
`./build/test_raft_reconcile`.

**Limitations:** no on-wire entries, network log replication, majority
commit tracking, persistence or state machine in this phase. Future network
handlers must hold the node mutex and check leader term before calling
`raft_log_reconcile`. The private learning PDF is outside the repository.

**Commit:** `feat: add conflict-safe in-memory raft log reconciliation`


## Phase 12 / 35 - AppendEntries entry batches over TCP

**Budget:** ~2 hours. **Planned slot:** January 12, 2027.

**Deliverable:** a C follower accepts a bounded, binary-safe batch of Raft
log entries over framed TCP, checks `(prev_index, prev_term)` under the node
mutex, repairs the uncommitted suffix, and replies success or failure.

- [x] Preserve the legacy 36-byte empty heartbeat format
- [x] Add entry-count and variable-length command serialization (<=4096 bytes)
- [x] Validate payload boundaries, term order, entry counts and membership
- [x] Deep-copy accepted commands into follower-owned memory
- [x] Reject stale terms, nonexistent prefixes and committed conflicts
- [x] Extend live TCP AppendEntries server handler
- [x] Add one-shot `--append-demo` and `--log-status` developer probes
- [x] Unit tests with binary data, malformed frames and rollback behavior
- [x] Real TCP integration tests for multiple entries and suffix repair
- [x] Run complete existing regression suite
- [ ] Validate on personal Fedora installation
- [ ] Review changes and commit with actual date and time

**Verify:** `make clean && make && make test`.

**Not yet complete:** elected-leader proposal path, nextIndex/matchIndex,
automatic retries, quorum commits, durable storage, replicated application.
Learning PDF remains external and must never be committed.

**Commit:** `feat: replicate bounded raft log batches over framed TCP`

## Phase 13 / 35 - Leader-side replication progress and retry

**Time budget:** ~2 hours  
**Planned milestone:** January 13, 2027 (use the actual completion date)

### Deliverable

Elected leaders track each peer's `nextIndex` and `matchIndex`, send bounded
AppendEntries batches instead of only empty heartbeats, decrease `nextIndex`
on a log mismatch and retry on later cycles. A development-only `--propose-demo`
command appends an uncommitted entry to the elected leader to make automatic
replication observable. This is **not** a production client proposal API.

### Acceptance criteria

- [x] Separate per-peer replication state, initialized on each leadership term
- [x] Request `prevLogIndex` and `prevLogTerm` reflect each follower's next index
- [x] Limit entry-bearing batches to 16 entries / 4096 encoded payload bytes
- [x] Success updates confirmed `matchIndex` and advances `nextIndex`
- [x] Failure backs off `nextIndex`, conservatively clearing stale match progress
- [x] Transport errors leave retry position unchanged
- [x] Higher-term response triggers step-down without applying stale progress
- [x] Encode borrowed command data under mutex, send TCP RPC outside mutex
- [x] Follower-only nodes reject development-only proposals
- [x] Real three-process integration covers offline follower and restart catch-up
- [x] Existing strong log/node regression checks remain enabled
- [x] All prior phase regression tests pass
- [ ] Run on personal Fedora hardware and review actual Git diff before commit

### Verify

```sh
make clean && make && make test
./build/test_raft_progress
python3 tests/test_progress_integration.py ./build/raft-engine
# Three terminals: --elect config/node1.conf, node2.conf, node3.conf
# After identifying the elected leader (example: 2):
./build/raft-engine --propose-demo config/node1.conf 2 "SET demo 13"
./build/raft-engine --log-status config/node1.conf 2
./build/raft-engine --log-status config/node1.conf 3
```

### Git message

```text
feat: track raft follower replication progress and retry log batches
```

**Scope limit:** `matchIndex` is a leader-side observation, not a commit index.
`commitIndex` does not advance on leader proposal. Nodes lose memory-only logs,
terms and votes on restart; diagnostic RPC lacks authentication and should be
used on loopback only. Majority-based commitment is Phase 14.


## Phase 14 / 35 - Current-term majority commit tracking

**Planned slot:** January 14, 2027 (the actual Git commit date should reflect reality).
**Target:** Convert leader-side `matchIndex` observations into Raft's
majority-backed `commitIndex` and deliver the commit watermark to followers.

- [x] Implement pure, mutex-external `raft_commit_advance()` helper
- [x] Include the leader as one voter and require both configured follower slots
- [x] Validate peer IDs, distinctness, same term and replication-index bounds
- [x] Select the 2nd-highest index among the leader and two followers
- [x] Directly commit only current-term majority entries
- [x] Preserve monotonic `commitIndex` across stale/failed observations
- [x] Feed updated `leaderCommit` through existing AppendEntries heartbeats
- [x] Follower caps commit index to successfully matched log prefix
- [x] Add 72 focused assertions and no-quorum/rejoin live TCP integration
- [x] Update Phase 13 live regression to expect eventual commits
- [x] Preserve the stronger legacy log/node test coverage and `"unknown"` role contract
- [x] Run full Phase 1–14 regression suite successfully
- [x] Run focused Phase 14 and Phase 13 integration checks under Clang ASan/UBSan
- [ ] Verify on personal Fedora installation
- [ ] Review the Git diff and commit with the actual timestamp

**Verification:** `make clean && make && make test`,
`./build/test_raft_commit`, and
`python3 tests/test_commit_integration.py ./build/raft-engine`.

**Boundary:** The quorum calculation is based on acknowledged *volatile* log
copies. No disk-backed safety or state-machine application is provided. The
leader's development-only proposal RPC returns before quorum commitment, and
`lastApplied` remains zero. Phase 15 will address the apply boundary.

**Suggested commit:** `feat: advance raft commit index using majority replication`


## Phase 15 / 35 - Ordered state-machine application (in-memory)

**Planned slot:** January 15, 2027. Use the actual completion time for Git.

### Delivered

- [x] Deterministic bounded `SET key value` / `DEL key` state machine
- [x] Binary-safe values, strict bounded ASCII key validation
- [x] Apply only committed log entries, in index order, exactly once
- [x] Track `lastApplied` as separate from `commitIndex` and `lastLogIndex`
- [x] Handle empty/unrecognized/over-limit committed commands as no-ops
- [x] Preserve retry safety on allocation failures before advancing index
- [x] Apply followers after successful AppendEntries and leaders after quorum checks
- [x] Synchronize state-machine reads and mutations under server mutex
- [x] Read-only wire diagnostics and `--apply-status`, `--get-demo` CLI
- [x] Unit tests for command grammar, binary values, idempotence and limits
- [x] Three-node TCP integration: commit, no quorum, DEL, recovery, malformed, replay
- [x] Keep legacy log/node regression tests and `"unknown"` role behavior
- [ ] Repeat complete suite on personal Fedora machine
- [ ] Review diff / use current real date for commit

**Verification:** `make clean && make && make test`,
`./build/test_raft_state_machine`, and
`python3 tests/test_apply_integration.py ./build/raft-engine`.

**Boundary:** The diagnostic proposal acknowledges mere admission; diagnostic
GET is not linearizable; neither log nor KV state survives restart without
replication from still-running nodes. WAL durability, crash recovery and real
client request/response guarantees remain future phases.

**Suggested commit:** `feat: apply committed raft log entries to in-memory state machine`


## Phase 16 / 35 - Durable term/vote and log snapshot groundwork

**Planned slot:** January 16, 2027. Use the actual date of your commit.

- [x] Versioned, portable big-endian metadata + ordered binary log format
- [x] CRC32 integrity detection; strict bounds and consistency validation
- [x] Per-node exclusive advisory lock to prevent overlapping writers
- [x] Temporary file, `fsync(file)`, `renameat`, `fsync(directory)`
- [x] Persist self-vote before RequestVote, and vote grants before acknowledgement
- [x] Persist leader proposals and follower log reconciliation before ACK
- [x] Persist term adoption and majority commit advancement
- [x] Fail-stop on persistence errors rather than serving unrecorded state
- [x] Opt-in `--elect-durable CONFIG DATA_DIR` leaving `--elect` unchanged
- [x] Recovery validates node ID/term/vote/log/commit, resets follower timeout
- [x] Deterministically rebuild KV from persisted committed log on startup
- [x] Unit tests for binary recovery, locks, CRC, truncation and replacement
- [x] Three-process SIGKILL / restart / quorum / corruption integration test
- [x] Preserve legacy log/node tests and prior consensus features
- [ ] Test with controlled power-loss, ENOSPC and hardware write barriers
- [ ] Run on personal Fedora machine and commit with the real timestamp

**Commands:** `make clean && make && make test`;
`./build/test_raft_storage`;
`python3 tests/test_storage_integration.py ./build/raft-engine`.

**Boundaries:** Full-state snapshots are O(size of log) per mutation; not a WAL.
Durability requires a filesystem that honors fsync/rename semantics, not NFS.
There is no safe node replacement, authenticated client API, linearizable
read, background compaction, snapshot installation, or membership change.

**Suggested commit:** `feat: persist raft term vote log and committed state atomically`


## Phase 17 / 35 — Append-only Write-Ahead Log & Incremental Recovery

**Time budget:** ~2 hours (planned Jan 17, 2027; record actual commit time)

### Deliverable

A separate opt-in WAL-backed three-node mode that writes term/vote/commit
changes and only newly appended or replaced log suffixes, never rewriting the
whole existing log on each update. Recovery replays a bounded, validated
sequence of durable records and reconstructs the KV state from committed logs.

### Acceptance checks

- [x] `--elect-wal CONFIG DATA_DIR` with shared per-node advisory locks
- [x] Format-versioned, node-bound, CRC32-protected WAL header
- [x] Length/CRC-framed records with strictly increasing sequence numbers
- [x] Metadata-only updates and prefix+suffix log edits, binary-safe commands
- [x] `fsync` record before vote/AppendEntries/proposal/commit acknowledgement
- [x] Replay retains committed log prefix and term/vote/commit monotonicity
- [x] Torn *final* record is truncated and fsynced; complete corruption fails
- [x] Same node/data directory cannot mix Phase 16 snapshot and Phase 17 WAL
- [x] Uncommitted suffix preserved but invisible after abrupt process restart
- [x] Dedicated C tests and three-process SIGKILL, tail and CRC tests
- [x] Targeted Clang ASan/UBSan unit and integration tests
- [ ] Run all tests on your own Fedora machine and review Git diff
- [ ] Commit with the actual date when you are satisfied

### Verify

```sh
make clean && make && make test
./build/test_raft_wal
python3 tests/test_wal_integration.py ./build/raft-engine
```

### Git commit

```sh
git add Makefile README.md progress.md docs/architecture.md \
    include/raft_wal.h include/raft_tcp.h \
    src/raft_wal.c src/raft_storage.c src/raft_tcp.c src/main.c \
    tests/test_raft_wal.c tests/test_wal_integration.py
git diff --cached --check
git commit -m "feat: add incremental raft WAL and crash-safe journal replay"
```

**Scope limits:** No compaction or log snapshots installed from leader;
64 MiB WAL cap; no online migration from Phase 16; no client linearizability.

**Next Phase 18:** WAL compaction / checkpoint creation and bounding replay time.


## Phase 18 / 35 — WAL checkpoints and crash-atomic compaction

**Planned milestone:** January 18, 2027 (record actual completion timestamp).
**Time budget:** ~2 hours.

### Deliverable

Single-file checkpointing that atomically replaces a long Phase 17 delta WAL
with an equivalent WAL header plus one full-state record, while retaining
all entries, including uncommitted log suffixes. Existing WAL data remains
compatible. A manual offline compaction CLI and automatic journal-length/
record-count thresholds bound the number of delta frames replayed.

### Acceptance checks

- [x] Existing Phase 17 WAL files remain readable without format change.
- [x] A new single-record checkpoint replays the same term, vote, log and commit.
- [x] Uncommitted log suffix is preserved and not applied on restart.
- [x] `fsync(temp)` -> atomic rename -> `fsync(dir)` before a successful checkpoint.
- [x] Stale temporary checkpoint ignored and removed under node lock.
- [x] Existing corruption validation rejects bad checksums before compaction.
- [x] Automatic compaction at 64 records or >=256 KiB with >=4 records.
- [x] Offline `--compact-wal CONFIG DATA_DIR` requires the exclusive WAL lock.
- [x] Commit/persistence semantics and old client interfaces remain unchanged.
- [x] Dedicated unit, live three-process crash/restart and sanitizer tests.
- [ ] Run the complete suite and inspect local Git diff on Fedora.
- [ ] Commit using the actual date after review.

### Verify

```sh
make clean && make && make test
./build/test_raft_checkpoint
python3 tests/test_checkpoint_integration.py ./build/raft-engine
# Stop any node before running its manual compaction:
./build/raft-engine --compact-wal config/node1.conf data
```

### Git commit: exact changed files

```sh
git add Makefile README.md progress.md docs/architecture.md \
    include/raft_wal.h src/raft_wal.c src/main.c \
    tests/test_raft_checkpoint.c tests/test_checkpoint_integration.py
git diff --cached --check
git diff --cached --stat
git commit -m "feat: atomically compact raft WAL into recoverable checkpoints"
```

**Scope limitation:** This does not truncate the actual Raft log. The disk
image grows with retained log entries; a future state-machine snapshot and
InstallSnapshot RPC are needed for real log-prefix compaction.

**Next:** Phase 19 — Raft state-machine snapshots and prefix-trimming design.


## Phase 19 / 35 - State-machine snapshots and log-prefix foundations

**Planned milestone:** January 19, 2027 (commit with actual completion date).

### Delivered

- [x] Binary-safe, bounded, checksummed v1 state-machine snapshot format.
- [x] Snapshot captures exactly the `lastApplied` index and corresponding term.
- [x] Atomic offline snapshot writes and strict, transactional decoding.
- [x] In-memory Raft log uses an absolute `(base_index, base_term)` boundary.
- [x] Safe prefix trimming through an applied snapshot, retaining suffix.
- [x] Restore applied KV state into a fresh node and replay later log entries.
- [x] Legacy log, storage and WAL tests remain passing.
- [x] Durable writers refuse nonzero compacted bases until format migration.
- [x] Offline `--snapshot-demo DIRECTORY`, snapshot tests and shell integration.
- [ ] Inspect Git diff and run `make test` on Fedora.
- [ ] Commit using actual date.

### Acceptance commands

```sh
make clean && make && make test
./build/test_raft_snapshot
mkdir -p /tmp/raft-p19
./build/raft-engine --snapshot-demo /tmp/raft-p19
```

### Commit staging - all Phase 19 additions and modifications

```sh
git add Makefile README.md progress.md docs/architecture.md \
    include/raft_log.h include/raft_snapshot.h \
    src/raft_log.c src/raft_node.c src/raft_storage.c src/raft_wal.c \
    src/raft_snapshot.c src/main.c \
    tests/test_raft_snapshot.c tests/test_snapshot_demo.sh
git diff --cached --check
git diff --cached --stat
git commit -m "feat: add raft state machine snapshots and log prefix boundaries"
```

**Limit:** Live Raft/WAL log truncation is *not enabled*. InstallSnapshot,
atomic snapshot/WAL generation migration, and replication catch-up from a
compacted leader are required for safe online compaction.

**Next:** Phase 20 - InstallSnapshot RPC and lagging-follower catch-up design.


## Phase 20 / 35 — InstallSnapshot RPC & Lagging-Follower Recovery

- [x] Encode/decode bounded 40-byte chunk requests and 13-byte responses.
- [x] Validate membership, term, CRC, snapshot metadata, offsets and lengths.
- [x] Support resumable in-memory assembly across multiple TCP connections.
- [x] Install KV state and snapshot `(index, term)` boundary while retaining a
      matching follower suffix and never overwriting a conflicting committed suffix.
- [x] Leader switches from AppendEntries to snapshot transfer when `nextIndex`
      crosses compacted `base_index`, then returns to ordinary replication.
- [x] Add opt-in `--elect-snapshot` and educational `--snapshot-compact-demo`.
- [x] Preserve the older `--elect-durable` / `--elect-wal` disk formats and tests.
- [x] Unit, three-node TCP integration, and targeted Clang sanitizer tests.
- [ ] Run locally on Fedora, inspect Git diff and commit using the actual date.

### Acceptance commands

```sh
make clean && make && make test
./build/test_raft_install
python3 tests/test_install_integration.py ./build/raft-engine
```

### Git staging — all Phase 20 additions / changes

```sh
git add Makefile README.md progress.md docs/architecture.md \
  include/raft_install.h include/raft_wire.h include/raft_tcp.h \
  include/raft_replicate_client.h \
  src/raft_install.c src/raft_wire.c src/raft_tcp.c \
  src/raft_replicate_client.c src/main.c \
  tests/test_raft_install.c tests/test_install_integration.py

git diff --cached --check
git diff --cached --stat
git commit -m "feat: transfer raft snapshots to lagging followers over TCP"
```

**Boundary:** Phase 20 snapshot mode is memory-only, unauthenticated and
educational. Full crash-safe snapshots integrated with WAL are not implemented.

**Next:** Phase 21 — Snapshot/WAL generation consistency and crash recovery.


## Phase 21 / 35 — Snapshot/WAL Generation Consistency and Crash Recovery

- [x] Define isolated snapshot-aware WAL v1 format, separate from Phase 16-18.
- [x] Persist snapshot bytes, `(baseIndex,baseTerm)`, consensus metadata,
      and retained suffix in one CRC-validated atomic checkpoint generation.
- [x] Append `fsync`ed incremental metadata/log-suffix journal deltas between
      checkpoints, and periodically consolidate the history.
- [x] Acknowledge InstallSnapshot completion or leader compaction only after
      an atomic combined checkpoint succeeds; fail-stop on storage failure.
- [x] Recover snapshot KV, replay committed suffix, leave uncommitted unseen.
- [x] Refuse corrupt complete frames, truncate torn final records, ignore
      orphan temp checkpoints, and lock the writer per node.
- [x] Preserve Phase 1–20 public modes and comprehensive legacy unit tests.
- [x] Unit tests and multi-process `SIGKILL`/restart integration tests.
- [ ] Run on Fedora, inspect staged diff, commit with actual work date.

### Phase 21 acceptance commands

```sh
make clean && make && make test
./build/test_raft_snapwal
python3 tests/test_snapwal_integration.py ./build/raft-engine
```

### Phase 21 Git staging (all new or changed files)

```sh
git add Makefile README.md progress.md docs/architecture.md \
  include/raft_snapwal.h include/raft_tcp.h \
  src/raft_snapwal.c src/raft_tcp.c src/main.c \
  tests/test_raft_snapwal.c tests/test_snapwal_integration.py
git diff --cached --check
git diff --cached --stat
git commit -m "feat: coordinate Raft snapshots with crash-safe WAL generations"
```

**Next:** Phase 22 — client request semantics and committed write acknowledgements.


## Phase 22 / 35 — Client Write Requests and Commit Acknowledgements

- [x] Add version-1 framed client-write messages and strict command/timeout parsing.
- [x] Accept commands only on an elected leader; reject follower and malformed writes.
- [x] Return `COMMITTED` only when original-term index is majority committed.
- [x] Return an explicit indeterminate `TIMEOUT`/`UNKNOWN` on incomplete operations.
- [x] Preserve durable/WAL persistence-before-ack paths and fail-stop behavior.
- [x] Test no-quorum timeout and later commitment of that same entry.
- [x] Test client-write recovery under snapshot-WAL using `SIGKILL`.
- [x] Preserve comprehensive Phase 1–21 log/node/network tests.
- [ ] Test on Fedora, inspect staged files, and commit with real date.

### Phase 22 commands

```sh
make clean && make && make test
./build/test_raft_client_write
python3 tests/test_client_write_integration.py ./build/raft-engine
python3 tests/test_client_write_durable.py ./build/raft-engine
./build/raft-engine --write config/node1.conf 2 2500 'SET alpha hello'
```

### Phase 22 staging list (all new or changed files)

```sh
git add Makefile README.md progress.md docs/architecture.md \
  include/raft_client_write.h include/raft_replicate_client.h include/raft_wire.h \
  src/raft_client_write.c src/raft_replicate_client.c src/raft_tcp.c \
  src/raft_wire.c src/main.c \
  tests/test_raft_client_write.c tests/test_client_write_integration.py \
  tests/test_client_write_durable.py
git diff --cached --check
git diff --cached --stat
git status
git commit -m "feat: acknowledge client writes only after raft majority commit"
```

**Next:** Phase 23 — request IDs, deduplication and safer retry semantics.

## Phase 23 / 35 — Client Request IDs & Deduplication

- [x] Define optional new wire types 23/24 and binary-safe request encoding.
- [x] Wrap client ID/request ID together with the original replicated command.
- [x] Maintain deterministic per-client last-applied session records.
- [x] Reject stale IDs and conflicting command reuse; prevent multiple in-flight operations per client.
- [x] Deduplicate pending requests without creating another log entry.
- [x] Preserve session records through snapshot v2, InstallSnapshot, and snapshot-WAL restart.
- [x] Keep snapshot v1 readable and legacy `--write` protocol compatible.
- [x] Add Phase 23 C unit tests, real-cluster leader replacement, timeout/retry, compaction and SIGKILL integration tests.
- [x] Preserve Phase 1–22 unit tests and run earlier network tests.
- [ ] Run on Fedora, inspect changes, and commit using the actual local date.

### Commands

```sh
make clean && make && make test
./build/test_raft_dedup
python3 tests/test_dedup_integration.py ./build/raft-engine
./build/raft-engine --write-id config/node1.conf 2 2500 81001 1 'SET alpha beta'
```

### Git staging list — all Phase 23 new or changed files

```sh
git add Makefile README.md progress.md docs/architecture.md \
  include/raft_dedup.h include/raft_replicate_client.h \
  include/raft_state_machine.h include/raft_wire.h \
  src/main.c src/raft_dedup.c src/raft_replicate_client.c \
  src/raft_snapshot.c src/raft_state_machine.c src/raft_tcp.c src/raft_wire.c \
  tests/test_raft_dedup.c tests/test_dedup_integration.py
git diff --cached --check
git status
git commit -m "feat: deduplicate raft client writes using replicated request IDs"
```

**Next:** Phase 24 — automatic leader discovery / client retries with bounded redirects.

## Phase 24 / 35 — Automatic Leader Discovery & Bounded Client Retries

- [x] Add a separate client-side router without changing existing RPC types.
- [x] Probe configured peers for role and term; reject malformed or mismatched status frames.
- [x] Choose a single leader at the highest observed term; handle split and stale responses.
- [x] Add `--write-auto CONFIG BUDGET_MS CLIENT_ID REQUEST_ID COMMAND`.
- [x] Reuse the same encoded identity/payload throughout all retries.
- [x] Retry `NOT_LEADER`, `TIMEOUT`, `UNKNOWN`, and transport failures within a monotonic deadline.
- [x] Return `COMMITTED` only on a server commit acknowledgement; stop on `REJECTED`.
- [x] Preserve legacy CLI and wire compatibility and comprehensive legacy log/node tests.
- [x] Add deterministic router unit assertions and three-node recovery/failover tests.
- [ ] Run on Fedora, review stage, and commit using actual completion time.

### Phase 24 commands

```sh
make clean && make && make test
./build/test_raft_client_auto
python3 tests/test_auto_client_integration.py ./build/raft-engine
./build/raft-engine --write-auto config/node1.conf 8000 91001 1 'SET order paid'
```

### Phase 24 Git changes (all nine new/modified files)

```sh
git add Makefile README.md progress.md docs/architecture.md \
  include/raft_client_auto.h src/raft_client_auto.c src/main.c \
  tests/test_raft_client_auto.c tests/test_auto_client_integration.py
git diff --cached --check
git diff --cached --stat
git status
git commit -m "feat: discover raft leaders and safely retry client writes"
```

**Next:** Phase 25 — authenticated client requests and stricter admission controls.
