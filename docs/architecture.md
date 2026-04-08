# Raft Engine Architecture - Through Phase 5

This document distinguishes what exists now from future Raft consensus.

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
