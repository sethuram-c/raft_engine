# Raft Engine Architecture - Through Phase 12

Historical sections below describe the earlier milestones; the current
Phase 12 architecture and protocol are summarized at the end.

## Modules

```text
                    src/main.c (CLI)
                     /   |      \
            raft_node   raft_config   raft_tcp
                 |           |             |
             raft_log        +--------- raft_wire
                 |
             raft_types.h
```

- `raft_types.h`: node ID, term, index, roles and common status types.
- `raft_log.c`: heap-owning binary-safe log, append/lookups/truncation.
- `raft_node.c`: local node initialization and invariant verification.
- `raft_config.c`: fixed three-node validated IPv4 configuration.
- `raft_tcp.c`: TCP listen/connect, socket timeouts, PING -> PONG handler.
- `raft_wire.c`: pure header encode/decode and exact-byte frame I/O.
- `main.c`: CLI for demo, config validation, listen/probe, ping.

## Wire protocol V1 (educational)

```text
byte offsets:      0       4  5     6       8           12
                  +---------+--+--+-----+-----------+----------------+
                  |  MAGIC  |V |TY| RSV |  PAYLOAD  | PAYLOAD bytes  |
                  |  u32 BE |1 |1 |u16=0| LEN u32BE | 0..4096 bytes  |
                  +---------+--+--+-----+-----------+----------------+
MAGIC = 0x52414654 ('RAFT'), V = 1, TY = 1 (PING) or 2 (PONG)
```

All integer serialization is explicit byte shifting. C structure layout is
**not** sent over the network, because padding/endianness would vary by ABI.
Payloads are arbitrary bytes, and no NUL terminator is transmitted.

## Listener behavior

```text
load & validate config -> listen -> poll -> accept
                                               |
                              configure 1s idle timeouts
                                               |
                            recv 12-byte header exactly
                                               |
                                validate / bound length
                                               |
                               recv payload exactly
                                               |
                           if PING => echo same bytes as PONG
                                               |
                                  close accepted socket
                                               |
                                        poll again
```

A `recv()` can yield fewer bytes than requested. The frame helper loops until
it has exactly the required bytes or detects an error or EOF. A zero return
from `recv` while collecting a payload or partial header is a truncated frame.
A zero return *between* frames is a normal connection close.

`MSG_NOSIGNAL` protects Linux sender processes from `SIGPIPE` when a peer
closes the socket; errors are returned explicitly. A framed request is handled
on **one connection**, then the server closes it. Other connections wait.

## Ownership and safety

- Transport functions do not own or close the socket passed to wire helpers.
- All accepted sockets are closed exactly once by the server.
- Incoming length must be <= 4096 before allocating/copying a payload.
- Output buffers/type/length are unchanged when frame receive fails.
- Bad protocol headers or disconnects are logged and the connection closed.
- SIGINT/SIGTERM handlers only update a `sig_atomic_t` stop flag.
- The 1-second SO_RCVTIMEO/SO_SNDTIMEO values bound *idle operations*, not
  an overall message deadline if a peer trickles bytes slowly.

## What is not implemented yet

No durable Raft term/vote, RequestVote RPC, AppendEntries RPC, heartbeat,
leader election, peer identity authentication, quorum logic, or key-value
state machine. PING/PONG is only proof that framing is reliable over TCP.

**Next phase:** Concurrent connection handling using POSIX threads and mutexes.


## Phase 6: Bounded concurrent TCP server

```text
              ┌────────── TCP listening socket ──────────┐
              │               Accept thread              │
              │ poll -> accept -> select free worker slot│
              └──────────────────┬───────────────────────┘
                                 │ pthread_create
                     ┌───────────┴────────────┐
                     │ 8 maximum worker slots│
                     │ worker -> recv PING    │
                     │        -> send PONG    │
                     │        -> close socket │
                     └───────────┬────────────┘
                                 │ mutex + condition
                              join & reuse
```

The accept loop owns the listener and worker slot allocation. Each worker
owns its accepted socket. The mutex synchronizes the workers' completed
flags and socket closure with the accept thread's stop-time `shutdown()`.
The condition variable wakes the accept loop when capacity becomes available.
A worker slot is **never reused before `pthread_join`**. Full capacity means
waiting rather than creating unlimited threads; pending connects may queue
in the operating system's listen backlog.

`--max-connections N` counts accepted sockets and normally waits for all
accepted work to drain before exit. On SIGINT/SIGTERM, the main thread
interrupts workers' sockets with `shutdown()`, joins threads and restores
the previous signal handlers. The signal handler itself only sets a
`volatile sig_atomic_t` flag.

Threat model: this is an educational localhost TCP demo with no auth or
Raft RPCs. One-second **idle** I/O timeouts do not bound the complete
request lifetime against slow clients.

## Phase 7: follower election timeout

`raft_timer.h` and `raft_timer.c` implement a single randomized deadline for
future Raft election triggering. `CLOCK_MONOTONIC` gives a real elapsed-time
sample, while `raft_election_timer_reset(timer, now_ms)` accepts caller-supplied
logical time to make boundary tests deterministic. A per-node xorshift32 state
produces timeouts in [350, 700] ms, inclusive. The random source is NOT
cryptographic; production election seeds should derive from OS entropy so
nodes do not repeat identical timeout sequences after restarts.

`raft_node_start_follower()` initializes/arms the timer and selects follower
role without modifying `current_term` or `voted_for`.
`raft_node_reset_follower_timeout()` re-arms the timer only while the node is
a follower. A future validated AppendEntries/RequestVote handler can call
this reset; raw bytes, failed requests and unverified network traffic must NOT
reset the follower election timer. `raft_node_follower_timeout_pending()`
reports a boolean and does not transition to candidate yet.

This library is not thread-safe: future Raft event-processing code must own
one node's state or protect node/timer operations under a mutex. The existing
Phase 6 TCP worker pool remains a separate PING/PONG demo and does not access
Raft node state. No heartbeat, election RPC, persistent vote, or consensus yet.

## Phase 8: RequestVote RPC (implemented)

One TCP connection carries one 12-byte RAFT frame header followed by a fixed
28-byte RequestVote request or 9-byte response. Request fields in network order:
`term:u64`, `candidate_id:u32`, `last_log_index:u64`, `last_log_term:u64`.
Response: `term:u64`, `vote_granted:u8` where the flag is 0 or 1. The
frame type is 3 (request) or 4 (response).

The receiver checks the candidate is a distinct configured cluster member,
decodes into a temporary struct, locks its state mutex, applies the Raft vote
rules, unlocks, then replies. The same mutex also guards concurrent worker
shutdown accounting; lock hold time is limited to memory-only state mutation.

Follower RequestVote rules: reject terms lower than `current_term`; first
adopt a higher term and clear the vote; grant only when the candidate log is
at least as up-to-date (last term first, then last index) and the voter has
not already voted for a different candidate this term. Granting resets the
follower election timer. Rejecting does not reset it. A higher term also
forces a leader/candidate to become follower even if the vote is rejected.

**Critical safety limitations:** `current_term` and `voted_for` are not
persisted to stable storage (planned later); process restarts can grant votes
again in the same term. Peer identity is inferred from a self-declared ID,
not authenticated; TCP is loopback demo transport. No candidate state machine,
quorum calculation, leader selection or AppendEntries exists yet. Thus Phase
8 is an RPC milestone, not a functioning consensus algorithm.


## Phase 9: election-control plane

In `--elect` mode an election thread shares a mutex-protected `raft_node_t`
with the bounded TCP RPC worker pool. Its timed loop reads CLOCK_MONOTONIC,
begins a candidacy when the node deadline expires, increments the current
term, self-votes and snapshots current last-log metadata. It makes outbound
RequestVote RPCs without holding the node mutex. Replies are applied under
the mutex; distinct, current-term granted votes count toward quorum (2/3).
A higher response term forces the node to follower and clears `voted_for`.

`--status` uses a framed type-5 zero-byte request and a type-6, 17-byte
response encoded in big-endian order: `node_id:u32 | term:u64 | role:u8 |
voted_for:u32`. This is diagnostic metadata only. It is neither authorized nor
linearizable and does not establish persistent cluster consensus.

**Deliberate gap:** A leader does not send AppendEntries heartbeats in this
phase. Other nodes may time out and start a new election. A returned leader
role is therefore not proof of stable leadership or durable election safety.


## Phase 10: zero-entry AppendEntries heartbeat protocol

Transport frame types **7** (AppendEntries) and **8** (response) extend the
Phase 5 12-byte RAFT outer header. Phase 10 requests are fixed 36 bytes:
`term:u64 | leader_id:u32 | prev_log_index:u64 | prev_log_term:u64 |
leader_commit:u64`; replies are fixed 9 bytes: `term:u64 | success:u8`.
All multi-byte fields are explicitly big-endian. This phase sends *no log
entries*. The same AppendEntries RPC will carry log entries in later phases.

In `--elect` mode, the election thread handles both candidacy and heartbeats.
After a successful majority vote, the newly elected leader sends its first
heartbeat on the next election-loop iteration and thereafter approximately
every 90ms. Sending is performed without holding the Raft state mutex. Each
client worker validates the leader ID against fixed membership before handling
the request. The state mutex guards follower term transitions, timer resets,
and responses. A higher-term heartbeat response causes immediate step-down.

Correct receiving rules for this zero-entry subset:

1. Reject a malformed request or nonmember before touching Raft state.
2. Lower-term requests receive `success=0` with the local term; timer unchanged.
3. For an equal/higher term, recognize the leader (become follower), reset the
   randomized election deadline and clear `voted_for` only on higher terms.
4. Compare the prev log index/term. A mismatch returns `success=0`, but *still*
   resets the election deadline because leader contact is valid.
5. With a matching prefix, a zero-entry heartbeat may move commitIndex up to
   `min(leaderCommit, prevLogIndex)`. It cannot invent missing entries.
6. The leader receiving a higher-term response reverts to follower and resets
   its timeout. Equality or stale replies do not force a transition.

The integration test uses three independent Linux processes, real temporary
loopback TCP ports, stability sampling, a stopped leader and a restarted node.
No persistent state, log conflict repair, authentication or quorum-loss
step-down is implemented at this milestone. Status sampling does **not** prove
linearizability or a network-wide leader lease.


## Phase 11 - Raft log reconciliation API (not yet on the wire)

The in-memory `raft_log_t` owns a dynamic array of `raft_log_entry_t` structs,
whose `command` buffers are independently heap-allocated. Log indices are
**1-based**; `(index=0, term=0)` is the virtual prefix. An empty log has
`last_index=0`, and `raft_log_last_term` returns zero.

`raft_log_reconcile(log, prev_index, prev_term, incoming, count,
committed_index, &matched)` performs the in-memory equivalent of the log
consistency and append portions of the AppendEntries follower algorithm:

1. Validate parameters and the entire proposed entry batch. Terms must be
   nonzero and nondecreasing; command pointers must be present for nonempty
   payloads; indices and allocation lengths cannot overflow.
2. Reject a missing or mismatched previous entry before changing anything.
3. Compare proposed entries with local entries by `(index, term)` and skip
   matching ones; a matching batch never truncates a local tail.
4. If the first conflict would overwrite an index <= `committed_index`, reject.
5. Allocate a new entry array, copy preserved ownership references, deep-copy
   replacement payloads, then atomically swap ownership into the log. If any
   allocation fails, free temporary copies, leave the old log intact.
6. Report the leader-proven last matching index as `prev_index + count`.

This API is **not synchronized internally**: when wired to AppendEntries in
Phase 12, the caller must hold the Raft mutex and validate sender membership,
terms, entry bounds and node state, then arrange durable writes before
acknowledging success. The current network AppendEntries request still carries
zero entries and is unchanged in Phase 11. `committed_index` is a protection
floor supplied by the caller; the function does **not** advance commitIndex.

Correct Raft leaders ensure identical commands for the same log index/term.
Because log matching relies on this invariant, this API compares overlapping
entries by term, not by bytewise command equality. Arbitrary hostile leaders
or disk corruption are outside this educational protocol's safety guarantees.

New files/tests: `tests/test_raft_reconcile.c`, `tests/test_log_demo.sh`.
Manual in-memory sample: `./build/raft-engine --log-demo`.


## Current architecture: Phase 12 log replication

```text
  CLI --append-demo (configured node 1, diagnostic probe)
        |
        | raft_append_encode_batch()
        v
  framed TCP (12-byte RAFT header, type=7)
        |
        v
  server worker on configured follower node 2
        |
        +-- verify wire framing + fixed cluster membership
        +-- raft_append_decode_batch() (command slices borrow socket buffer)
        +-- pthread mutex (node state and log serialization)
        |      |
        |      +-- stale/higher term rules and follower timeout reset
        |      +-- raft_log_reconcile() (deep-copy, atomic suffix replace)
        |      +-- commit_index = min(leader_commit, last_new_index)
        |
        +-- response: type=8, term:u64 + success:u8
        v
  diagnostic CLI displays result; --log-status reports metadata
```

- `src/raft_append.c`: validates, encodes, decodes and handles bounded entry
  batches; empty 36-byte heartbeats are backwards compatible.
- `src/raft_log.c`: owns all stored command bytes; incoming decoder borrows
  command slices only until reconciliation finishes.
- `src/raft_tcp.c`: validates cluster membership before locking the node,
  releases the mutex before sending the reply, and guards log state with
  the same mutex that protects election and voting state.
- `src/raft_replicate_client.c`: developer-only one-shot AppendEntries probe
  and read-only log-index status request (frame types 9/10).

### Phase 12 variable-length AppendEntries payload

```text
term:u64 | leader_id:u32 | prev_index:u64 | prev_term:u64 | leader_commit:u64
[ if entry-bearing: count:u16 | (term:u64 | len:u16 | command:len bytes)* ]
```

All integers are network-order. Total payload <= 4096 bytes; count <=16.
No C structs, pointers or native padding are transmitted. A 36-byte payload
means zero entries. 37-byte and malformed entry-bearing payloads are rejected.

Important distinctions:

- `success=1` means this follower matched the prefix and appended/reused the
  entries; it is NOT a quorum commit acknowledgement.
- `leader_commit` is an input from a trusted leader, not inferred from a
  single follower reply. The demo leaves it at zero.
- Automatic leader-side replication progress and retries are **not yet**
  implemented. Existing `--elect` heartbeats remain empty RPCs.
- State is not persisted and RPC senders are not authenticated. Bind to
  loopback addresses and never deploy in an untrusted network.
- The log status endpoint reports index/term/commit, not complete commands.

## Phase 13 - Per-follower leader replication progress

`--elect` mode now uses a leader-side AppendEntries replication loop instead
of sending only empty heartbeats. The existing bounded TCP worker pool receives
AppendEntries and reconciles each follower log just as in Phase 12.

```text
                       elected leader
                    node + log under mutex
                              |
                 progress[peer] (term, nextIndex, matchIndex)
                              |
              raft_replication_prepare()  <-- log slices borrowed
                              |
                     encode batch under lock
                              |
                            unlock
                              |
               framed TCP AppendEntries request
                              |
                   follower consistency check
                              |
                       AppendEntriesResponse
                              |
                     lock current-term node
                              |
            +-----------------+------------------+
       reply.term higher  reply.success=1   reply.success=0
        leader steps down   match=sentThrough   nextIndex--
                           next=match+1     match<=next-1
```

`include/raft_replication.h` and `src/raft_replication.c` are transport-free:

- `raft_replication_progress_init` seeds `nextIndex=leader_last+1`, `matchIndex=0`.
- `raft_replication_prepare` creates a bounded `raft_append_batch_t` from
  `nextIndex`, deriving `(prev_index,prev_term)` from the leader's log.
  Borrowed pointers are encoded while the node mutex is held.
- `raft_replication_observe` advances progress only for replies in the same
  term and backs off on rejections; network failures do not move indices.
- A newly elected leader initializes all non-self progress slots afresh.
- `raft_append_handle_response()` handles higher terms before any progress
  update. The election loop rejects replies if leadership/term changed.

The existing heartbeat interval is approximately 90ms; a peer whose
`nextIndex` is beyond the log tail receives a 36-byte empty AppendEntries
heartbeat, while a peer with missing entries receives up to 16 bounded entries.
Rejections cause `nextIndex` to decrement (not below 1); the next loop retries
from an earlier preceding entry. This is deliberately simple linear backoff,
not optimized conflict-hint backtracking or pipelining.

### New development-only proposal RPC

Wire types **11 (DEMO_PROPOSE)** and **12 (DEMO_PROPOSE_RESPONSE)** allow the
CLI `--propose-demo CONFIG LEADER_ID COMMAND` to append a 1–1024-byte raw
command **only to an elected leader**, guarded by the same node mutex. The
response is `term:u64 | last_index:u64 | accepted:u8` (17 bytes). This operation
is intentionally unauthenticated, loopback-only and not a consensus commit;
`commit_index` stays unchanged. It is provided solely to exercise automatic
leader-to-follower replication before the production client API phase.

Use `--log-status CONFIG PEER_ID` to inspect each peer's
`(last_index,last_term,commit_index)`. The test harness proves that a restarted,
empty in-memory follower causes a prefix mismatch, triggering repeated
backoff and successful full catch-up. It also tests that the healthy follower
continues progressing while one peer is stopped.

### Concurrency and lifetime

Every access to the leader's log and replication array uses `server->mutex`.
`raft_append_batch_t.entries[i].command` points into the leader's log and may
be invalidated by mutation; therefore `raft_append_encode_batch` completes
before the mutex is released. `raft_wire_send`, `raft_wire_recv` and socket
connect are performed after the mutex is released, allowing worker threads to
serve inbound RPCs. On return, the election thread rechecks role and term;
late replies cannot grant progress to a different leadership term.

### What Phase 13 does not do

No majority-based commitment, state-machine application, stable storage,
authenticated clients, membership change, conflict-hint optimization or
production-grade request pipelining. The uncommitted demo entries disappear
when a process restarts. `matchIndex` is not by itself proof that a command
is committed or safe to expose to a client.
