# Raft Engine Architecture - Through Phase 14

The early sections are preserved as **historical phase snapshots**. For the
current behavior, refer to the Phase 14 summary at the bottom of this file.

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


## Phase 14: majority commit flow (current implementation)

```
Development proposal --> leader in-memory log, index N
                            |
                periodic leader replication (Phase 13)
                            |
          follower AppendEntries success, same term
                            |
           mutex: update follower matchIndex
                            |
      raft_commit_advance(leader, progress[2])
                            |
       majority index = median(leader last, match2, match3)
                            |
       is majority index > commitIndex AND
         log[majority index].term == currentTerm ?
                       /          \
                     YES          NO
                     /              \
     commitIndex = majority      no change
                     |
        next replication heartbeat carries leaderCommit
                     |
       follower checks matching prefix, then caps commit
```

**API:** `include/raft_commit.h` / `src/raft_commit.c` owns the deterministic
three-voter quorum algorithm. The caller must hold the existing TCP server
mutex. Input is a validated leader and two distinct, current-term follower
progress records. The output is the updated leader `commitIndex`, but the
function does not apply commands, touch `lastApplied`, or perform I/O.

**Integration:** `src/raft_tcp.c` serializes reply handling, replication
progress updates and commit calculation under one mutex. On a successful
same-term follower acknowledgement, the progress array is copied under lock
and the majority index is calculated; an update is printed only when the
watermark increases. Subsequent `raft_replication_prepare()` calls include
`leaderCommit` in existing AppendEntries headers, without adding wire types.

**Invariant:** `0 <= lastApplied <= commitIndex <= lastLogIndex`. Direct
commit of an older-term entry is forbidden, even if it exists on two of three
nodes. A newly committed current-term entry indirectly commits all preceding
entries. Follower commit advancement is bounded by the successfully matched
prefix of its current AppendEntries RPC. No new commit is possible on the
leader's unilateral acknowledgement.

**Tests:** `tests/test_raft_commit.c` (72 assertions),
`tests/test_commit_integration.py` (three processes, quorum, lost quorum,
rejoin), plus a Phase 13 regression adapted to eventual commit propagation.
All previous Phase 1–12 checks are retained.

**Security/crash caveat:** The existing diagnostic proposal path has no
identity/authentication checks. This implementation has no disk persistence;
restart loses acknowledgements, committed entries, and term/vote state.
Therefore a majority in the current prototype is NOT a durable quorum.
Do not use it for data where Raft's real crash-safety guarantees are needed.
The key-value apply loop, client acknowledgement after commit, durable WAL,
snapshots and crash recovery are future milestones.


## Phase 15: committed state-machine application and diagnostic reads

`include/raft_state_machine.h` / `src/raft_state_machine.c` implement a
small deterministic in-memory KV state machine. A follower invokes
`raft_apply_committed()` **after successful AppendEntries**, while the elected
leader invokes it **after checking majority progress**. Both run under the same
`server->mutex` protecting the Raft node, log, replication progress and KV
store; there is no network I/O or blocking allocation while a TCP RPC is in
flight beyond the short mutex-protected critical section. The leader's
admission-only development proposal does NOT call apply.

The fundamental ordering invariant is:

```
0 <= lastApplied <= commitIndex <= lastLogIndex
for i = lastApplied+1 .. commitIndex (in order):
    execute log[i] on the KV state machine
    if processing succeeded: lastApplied = i
    else: stop and retry later; never mark i applied
```

The loop is naturally idempotent across repeated heartbeats: if
`lastApplied == commitIndex`, no commands are run. SET allocates a fresh
binary-safe value **before** replacing any existing buffer; if allocation
fails, it leaves both the store and lastApplied unchanged. DEL frees an
existing value, compacts the bounded key table and treats removal of an
absent key as successful. Malformed, unsupported and over-limit commands
have deterministic no-op semantics and increment `ignored_commands` while
still consuming a committed log index. This policy is a teaching choice; it
is not a recommendation for a real protocol's error response contract.

New frame types use the existing 12-byte envelope (network byte order):

| Type | Purpose | Payload |
|---|---|---|
| 13 | APPLY_STATUS_REQUEST | empty |
| 14 | APPLY_STATUS_RESPONSE | lastApplied:u64, commit:u64, keyCount:u32, ignored:u64 (28 bytes) |
| 15 | DEMO_GET_REQUEST | key (1..64 safe ASCII bytes) |
| 16 | DEMO_GET_RESPONSE | found:u8, lastApplied:u64, valueLen:u16, raw value bytes (11..1035 bytes) |

The `--apply-status` and `--get-demo` CLI commands use these diagnostics.
A `GET` reads one node's *local* state without a quorum or read-index
protocol, and can be stale even when it reaches a leader. It is not a
linearizable read and does not imply durable writes. These unauthenticated
RPCs must not be exposed on an untrusted network.

**Crash caveat:** a restarted process initializes an empty volatile log and
empty volatile KV state. Replication repairs the log and replays committed
entries during catch-up; the state is NOT durable and there is no write-ahead
log, fsync, snapshot, membership changes, real client API or crash recovery.
The phrase "majority committed" still means a majority of volatile copies;
Raft's real persistence safety guarantees are not implemented.

**Verification:** `tests/test_raft_state_machine.c` covers committed gating,
SET/DEL, binary values, duplicate application, invalid inputs, command limits
and table-full behavior. `tests/test_apply_integration.py` runs three live
processes through leader election, committed application, quorum loss,
recovery, malformed commands and restarted-follower replay. The existing
Phase 1-14 regression suite remains active.


## Phase 16 - Durable election and log recovery (opt-in)

```text
                     --elect-durable CONFIG DATA_DIR
                                   |
                         raft_storage_open()
                                   |  advisory flock node-ID.lock
                         raft_storage_load()
                                   |  strict magic/version/node/CRC/bounds
                          term, vote, log, commit
                                   |
                        follower timer reset
                                   |
                         apply committed prefix
                                   |
                            network listeners

 consensus event -- node mutex --> node updated
                                |
                       raft_storage_save()
                                |  serialize BE + CRC32
                  write temp --> fsync(temp) --> rename
                                |                     
                       fsync(directory)
                                |
                 unlock / send ACK or RequestVote
```

The portable on-disk bytes are:

```text
0..7   magic ASCII "RAFT16DB"
8..11  version u32 BE (1)
12..15 node id u32 BE
16..23 current term u64 BE
24..27 voted for u32 BE (0 means none)
28..35 commit index u64 BE
36..39 entry count u32 BE
then for each entry: term u64 BE, command length u32 BE, raw command bytes
last 4 bytes CRC32 IEEE covering everything preceding the CRC
```

Only the durable mode calls storage hooks. Every mutation that can affect a
peer-visible consensus decision is checkpointed before its associated RPC
response or subsequent request. Ordinary heartbeats with unchanged metadata
do not rewrite files. Disk failure requests fail-stop behavior; a file already
renamed but without successful directory fsync may or may not survive power
loss, so the process does not continue on that ambiguity.

The snapshot does not contain role (always restart FOLLOWER), leader peer
progress, KV contents, or `lastApplied` (replay through commit index). The
committed prefix is replayed with `raft_apply_committed` before accepting TCP
connections. Corrupt files are rejected, never overwritten with empty state.

This is an educational atomic image, not an append-only write-ahead log. It
can become expensive with large logs, has a deliberately strict 4 MiB cap, and
does not handle safe replacement of a lost node identity or linearizable reads.


## Phase 17 architecture — Incremental WAL and journal replay

The Phase 17 mode is opt-in: `--elect-wal CONFIG DIRECTORY`. The Phase 16
`--elect-durable` snapshot backend remains available with a separate directory.
Both modes use `raft_storage_open()` to lock the node ID, but WAL and snapshot
files for the same ID cannot be present in the same directory. This prevents
silent creation of independent durable consensus histories.

### Record format (portable, not C struct layout)

```text
WAL HEADER, exactly 20 bytes
offset       0       8       12      16
             +--------+-------+-------+-------+
             |RAFT17WL|VER=1  |NODE ID|CRC32  |
             | 8 bytes|u32 BE |u32 BE |u32 BE |
             +--------+-------+-------+-------+

EACH RECORD: [length:u32][CRC32:u32][payload:length bytes]
PAYLOAD:     sequence:u64, term:u64, voted_for:u32, commitIndex:u64,
             preserved_prefix_count:u32, appended_suffix_count:u32,
             repeated suffix entries: [entryTerm:u64, commandLen:u32, bytes]
```

The WAL header CRC covers its first 16 bytes; each record CRC covers its
payload. All multibyte integers use network byte order. A length cap, log
entry-count/command limits, sequential record sequence numbers, and Raft
term/vote/commit/log invariants are validated during replay.

### Save pathway

```text
node mutex acquired
  -> Raft mutation (term/vote/log/commitIndex)
  -> compare against in-memory copy of last fsynced consensus state
  -> find byte-identical shared log prefix
  -> encode ONLY changed metadata and suffix (no-op if nothing changed)
  -> preallocate next mirror state + WAL record
  -> append frame -> fsync(journal fd)
  -> swap in mirror; release node mutex; respond to peer
```

The disk mirror is replaced only after successful `fsync`. A save failure is
fatal: the node stops serving and no consensus acknowledgement is emitted.

### Recovery pathway

```text
lock ID + open WAL, refuse conflicting Phase16 snapshot
  -> validate magic/version/node ID/header CRC
  -> scan each frame: bound length -> full frame -> CRC -> seq -> metadata
  -> replay prefix+suffix into temporary node and validate invariants
  -> if ONLY incomplete tail: ftruncate to last good frame -> fsync WAL
  -> otherwise on corruption: reject startup WITHOUT resetting state
  -> reset to follower and randomized monotonic timer
  -> apply committed prefix to fresh in-memory KV table
  -> serve TCP with a memory mirror of last durable state
```

WAL size is limited to 64 MiB; current WAL replay builds a validated candidate
copy for each delta and can take time for long histories. There is no compaction
or automatic upgrade from a Phase 16 full snapshot yet. Entries may be present
but uncommitted after restart; such entries are never applied merely by replay.

### Safety boundaries

- Crash recovery tested with `SIGKILL`, including binary SET/GET and uncommitted
  log suffixes; filesystem or power-loss fault injection is not implemented.
- CRC32 detects accidental corruption, not hostile tampering.
- An incomplete *last* record is discarded; a complete corrupt record blocks
  startup. The WAL itself is append-only except for torn-tail recovery.
- Nodes cannot safely delete/recreate their own journal with the same voting
  identity. Cluster reconfiguration and safe node replacement remain future work.
- `--propose-demo` still acknowledges append admission, not commitment;
  `--get-demo` is a local, unauthenticated and non-linearizable diagnostic.


## Phase 18: Single-file crash-atomic journal checkpoints

The Phase 17 length/CRC-framed WAL remains the only authoritative durable
image. On compaction, the node writes a **new WAL file** containing the same
20-byte v1 header and exactly one full-state record: sequence `1`, current
term, voted-for ID, commit index, `keep=0`, and every entry in the log. The
record format deliberately stays compatible with the ordinary v1 replay path.

```text
persistent node-ID.lock (flock)
     |
     +-- live node-ID.wal: header + [delta 1][delta 2]...[delta N]
     |                                 | valid and already fsynced
     +-- node-ID.wal.compact.tmp: header + [full checkpoint]
                                         | write/fsync
                                         V
                                   renameat(tmp, live)
                                         |
                                   fsync(directory)
                                         |
                            old fd closed, new fd retained
                            sequence reset to 1
```

**Invariant:** the complete log, term/vote and committed index are preserved
verbatim; the checkpoint discards **history of WAL updates**, not log entries.
A smaller file reduces re-read frames. The state machine itself is rebuilt
from committed entries at startup, just as before Phase 18.

**Why one file?** Splitting checkpoint and WAL into two separately renamed
files requires generation numbers and a recoverable two-file transaction.
A single atomic file replacement preserves a clear old/new recovery choice.
A post-rename directory-sync error fails closed, since its durable outcome
could be ambiguous on a real filesystem.

**Concurrency/locking:** the election/TCP server already holds its node mutex
when persisting changes; the WAL also holds its per-node `flock`. The offline
`--compact-wal` command uses the same lock and refuses a live node. No external
RPC is added and no read or write operation bypasses WAL replay validation.

**Boundaries:** WAL format v1 is unchanged, and automatic compaction occurs at
64 records or >=256 KiB with >=4 records. The full log remains in each
checkpoint. There is no log-prefix discard, snapshot replication,
`InstallSnapshot` RPC, state-machine-only checkpoint, or WAL migration from
Phase 16. CRC checks detect accidents, not malicious tampering. POSIX fsync
and rename semantics depend on the local filesystem and hardware.


## Phase 19: state-machine snapshots and virtual log prefix

Raft log indices are **absolute**, even after compacting early entries. The
`raft_log_t` structure adds `(base_index, base_term)` for the virtual retained
entry, with an array for **only** the entries after that boundary.

```text
logical Raft indexes:     1    2    3    4    5
                               ^            ^
       snapshot boundary: (3, term=2)      last=5
retained in-memory data: [4, term=2] [5, term=3]
term_at(3)=2; at(3)=NULL; at(2)=NULL; at(4)=entry
last_index = base_index + count = 3+2 = 5
```

`raft_log_compact_prefix` verifies the boundary index/term and frees the old
commands before moving the retained suffix. Callers are responsible for the
**consensus precondition** `boundary <= lastApplied <= commitIndex`; the safer
`raft_snapshot_compact_applied` enforces this and matches a captured snapshot.
Normal un-compacted logs remain `base_index=0, base_term=0` and preserve all
previous index semantics. Reconciliation, truncate, and term lookup use the
absolute base offset; a compacted entry is never exposed as a command.

The Phase 19 `raft_snapshot_t` stores:
- `last_index, last_term` representing exactly the last *applied* log entry
- `executed_commands, ignored_commands` to preserve diagnostic counters
- a bounded table of unique validated ASCII keys and arbitrary binary values

Its standalone disk envelope uses 8-byte `RAFT19SN` magic, a 32-bit version
(1), 64-bit last index, last term, executed and ignored counts, and a 32-bit
key count (48 bytes total). Each record contains 16-bit key length, 16-bit
value length and the actual bytes, followed by CRC32 over the entire prefix.
All numeric fields are big-endian. The decoder validates strict lengths,
unique keys, max 128 keys, 64-byte keys, 1,024-byte values, counters, checksum,
and trailing data, and only then replaces the caller's snapshot. The writer
performs `write -> fsync(file) -> renameat -> fsync(directory)` in a trusted
directory. The file-level API requires callers to serialize writers; the
Phase 19 CLI is exclusively an isolated lab, not a live service.

**Crash safety boundary:** The snapshot format and in-memory compaction are
NOT wired to Raft's active storage. A durable log compaction transaction must
commit a snapshot and its suffix metadata together; otherwise a crash can
leave missing committed entries. Previous Phase 16/17/18 files do not carry
base index/term. Their writers deliberately reject nonzero bases.
A follower too far behind the leader's base needs an `InstallSnapshot` RPC
instead of a regular AppendEntries retry. Elections must compare absolute last
index/term, and live leaders must not trim without peer catch-up support.
Those features are postponed. The demo saves a separate snapshot file, trims
only its local test log, and restores into a *fresh* in-memory node before
applying a retained suffix. CRC32 is not authentication.
