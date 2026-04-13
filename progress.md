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
