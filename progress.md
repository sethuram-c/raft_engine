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
