# Raft Engine

A learning-oriented distributed key-value store under construction in **C11**
on **Fedora Linux**, using **GNU Make** and a fixed three-node configuration.

**Current milestone: Phase 15 / 35 - Ordered in-memory state-machine application.**
This three-node demonstration can elect a leader, maintain heartbeats, replicate
log batches with backtracking, compute majority-backed commits, and now apply
committed entries in order to a deterministic bounded key-value table.
The new commands are `SET key value` and `DEL key`. The state machine accepts
binary values (including NUL) over the existing AppendEntries protocol.
Only entries through `commitIndex` are visible. A malformed committed command
is a deterministic no-op but still advances `lastApplied`.

**This is not a production KV service.** The demo proposal RPC returns on log
admission, not on commitment. Reads are unauthenticated, local, and not
linearizable. Logs, votes, commits and applied state are ALL volatile: a
restarted node loses everything until it catches up. Only run on trusted loopback.

### Phase 15 quickstart

```sh
make clean && make && make test
# Three separate terminals:
./build/raft-engine --elect config/node1.conf
./build/raft-engine --elect config/node2.conf
./build/raft-engine --elect config/node3.conf
# Fourth terminal (example assumes node 2 was elected leader):
./build/raft-engine --status config/node1.conf 2
./build/raft-engine --propose-demo config/node1.conf 2 'SET greeting hello'
./build/raft-engine --apply-status config/node1.conf 2
./build/raft-engine --get-demo config/node1.conf 2 greeting
./build/raft-engine --log-status config/node1.conf 3
```

For a manual replication demonstration, wait until `last_applied` reaches the
new proposal index, then inspect the local key on all nodes. A response of
`accepted=1` is **not** evidence that the new entry is committed.

```sh
./build/test_raft_state_machine
python3 tests/test_apply_integration.py ./build/raft-engine
```

The `--get-demo` output is hexadecimal (`value_hex=`) so embedded NULs are
unambiguous. Command grammar is exact, case-sensitive and ASCII for keys:
`SET <key> <value>` or `DEL <key>`. Keys use `[A-Za-z0-9_.-]` with length
1..64; the value may have 0..1024 bytes, and the key table holds at most 128
entries. Unsupported or over-limit commands deterministically count as ignored
entries. An allocation failure stops applying without advancing past the failed
index; a later AppendEntries or heartbeat retries the pending work.

The older sections below document preceding phases historically; their earlier
"not implemented yet" scope statements should be read in their phase context.

## Implemented through Phase 8

- Raft term/node/index types, heap-owned in-memory log and node lifecycle.
- Strict three-node IPv4 configuration parser with uniqueness checks.
- TCP listen/accept, bounded nonblocking connect, clean SIGINT/SIGTERM shutdown.
- A 12-byte, network-byte-order header; exact-length send/receive helpers.
- Wire header validation: magic, protocol version, type, reserved bytes and
  maximum payload size (4096 bytes).
- Concurrent one-request-per-connection PING/PONG echo with up to 8 POSIX
  worker threads, mutex-protected completion, and condition-variable wakeups.
- The accept loop waits for a free worker instead of creating unlimited threads.
- Graceful worker cleanup on SIGINT/SIGTERM, including socket shutdown and joins.
- Randomized, monotonic follower election deadlines (350-700 ms) with
  deterministic, clock-injected tests; no candidacy or vote RPCs yet.
- RequestVote request/response encoding (28/9-byte big-endian payloads)
  over framed TCP types 3/4; strict size and domain validation.
- Follower vote decisions: terms, single vote per term, candidate log freshness,
  and election-timeout reset only on grant; term updates even on stale-log denial.
- Shared follower vote state protected by the TCP server mutex; non-member and
  self-as-candidate IDs rejected at the transport boundary.
- C unit tests and Python real-socket integration tests, including deliberately
  fragmented TCP writes, binary payloads, malformed/truncated frames,
  simultaneous clients, worker saturation, and signal-driven shutdown.

## Prerequisites (Fedora)

```sh
sudo dnf install -y gcc make python3 git gdb valgrind
# Optional for inspection: sudo dnf install -y iproute strace tcpdump
```

## Build and test

```sh
make clean
make
make test
make check-config
make demo
```

For debug diagnostics (separate from a normal build):

```sh
make clean
make CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined' LDFLAGS='-fsanitize=address,undefined'
make test CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined' LDFLAGS='-fsanitize=address,undefined'
make clean
make
```

`make test` runs Phase 1-15 suites. Loopback tests select temporary endpoints,
so example ports 7001-7003 do not have to be free.


## Phase 8: send one RequestVote request

Terminal 1 (peer 1 as a listening follower):

```sh
./build/raft-engine --serve config/node1.conf
```

Terminal 2 (configured candidate 2 requesting a vote):

```sh
./build/raft-engine --request-vote config/node2.conf 1 1 0 0
# Vote response: local_node=2 peer=1 term=1 granted=1
./build/raft-engine --request-vote config/node3.conf 1 1 0 0
# Vote response: local_node=3 peer=1 term=1 granted=0
./build/raft-engine --request-vote config/node3.conf 1 2 0 0
# Vote response: local_node=3 peer=1 term=2 granted=1
```

The arguments are LOCAL_CONFIG PEER_ID TERM LAST_INDEX LAST_TERM. The local
node ID in the configuration is the candidate ID. The example ports are fixed,
so stop any previous demo listening on port 7001 first. This demo does NOT
start a campaign, gather a quorum or elect a leader; it sends one request.
The in-memory vote is lost when the server restarts (durability comes later).

To run only the new tests:

```sh
make build/test_raft_vote
./build/test_raft_vote
python3 tests/test_vote_integration.py ./build/raft-engine
```

## Run the Phase 7 follower timer demonstration

```sh
./build/raft-engine --timer-demo config/node1.conf
./build/test_raft_timer
```

Expected output shape (randomized timeout values may differ with seeds):

```text
Follower timer: node=1 role=follower timeout_ms=519
At deadline-1: timeout_pending=0
At deadline: timeout_pending=1
After reset: timeout_pending=0 next_timeout_ms=501
Demo only: no candidate transition, voting, or leader election yet.
```

`--timer-demo` samples CLOCK_MONOTONIC once, then injects artificial timestamps
for repeatable boundary checks. It does not run a background timer, start a
cluster, send vote requests, or elect a leader. A seed derived from node ID
is used only to make this CLI example reproducible; production seeds will use
OS entropy in a later phase.

The timeout uses milliseconds, never wall-clock dates. The phase does not
integrate election timer mutation into the concurrently running TCP server:
that requires a serialized Raft event loop or explicit synchronization.

## Run the Phase 6 concurrent PING/PONG demo

Terminal 1:

```sh
./build/raft-engine --serve config/node1.conf
```

Terminal 2:

```sh
./build/raft-engine --ping config/node2.conf 1 hello-raft
```

Expected client output:

```text
PONG: local_node=2 peer=1 bytes=10 message=hello-raft
Demo only: no Raft RPC or consensus performed.
```

The listener reports `Accepted:` and `Replied:` events. Press `Ctrl+C` in
Terminal 1 to stop it. `--serve` accepts one framed request per connection,
replies only to PING and closes the socket. It handles up to eight clients concurrently; excess connections wait
in the kernel TCP accept backlog while workers are busy. On SIGTERM or
SIGINT, active sockets are interrupted and all workers are joined.

Try several concurrent pings in a second terminal:

```sh
for i in $(seq 1 6); do ./build/raft-engine --ping config/node2.conf 1 "request-$i" & done
wait
```

The earlier Phase 4 connection-only command still works:

```sh
./build/raft-engine --probe config/node2.conf 1
```

`--probe` does not send a frame. The listener closes it without sending a PONG.

## The Phase 5 wire format

Every frame is exactly **12-byte header + payload**:

| Offset | Bytes | Meaning |
| --- | ---: | --- |
| 0 | 4 | Magic `0x52414654` (ASCII `RAFT`) |
| 4 | 1 | Version: `1` |
| 5 | 1 | Type: `1=PING`, `2=PONG` |
| 6 | 2 | Reserved: must be zero |
| 8 | 4 | Unsigned payload length, big-endian (0-4096) |
| 12 | N | Opaque binary payload, not NUL terminated |

Example `PING` for `abc` in hexadecimal:

```text
52 41 46 54  01 01  00 00  00 00 00 03  61 62 63
```

This wire encoding is a **teaching/demo transport**, not the final Raft RPC
schema. Future RPCs will add term, node ID, log indices and message variants.

## CLI

```text
raft-engine --version
raft-engine --demo
raft-engine --check-config PATH
raft-engine --serve PATH [--max-connections POSITIVE_INTEGER]
raft-engine --probe PATH PEER_ID
raft-engine --ping PATH PEER_ID MESSAGE
raft-engine --timer-demo PATH
```

## Limitations and trust model

- IPv4 literals only; sample configs use `127.0.0.1`.
- Bounded thread-per-connection model (8 workers); this is not a production
  event-driven reactor or independently sized job queue.
- 1-second socket **idle** timeouts are not total frame deadlines; a slow
  client may stay connected by trickling bytes, so use localhost only.
- No peer authentication, TLS, persistent connections or replay protection.
- Protocol errors close only the affected connection; no error frame is sent.
- PING and PONG are demonstrations, not Raft RPCs or health guarantees.
- TCP packet boundaries do not equal message boundaries; reassembly is done
  by `raft_wire_recv`, and multiple frames can be decoded from a stream.
- Do not expose this learning server to untrusted networks.

The detailed phase learning PDFs are **separate local artifacts** and is not
stored in this repository. `.gitignore` excludes PDF and `learning-guides/`.

## Worker ownership model (Phase 6)

- The accept thread exclusively owns the listening socket and worker table.
- A worker owns exactly one accepted client socket until it closes it.
- The `pthread_mutex_t` protects socket-close/shutdown coordination and
  the `finished` flags; `pthread_cond_t` signals a newly free slot.
- Worker slots are reused only **after `pthread_join`**, preventing leaks.
- `--max-connections N` limits **accepted** connections, not simultaneous
  threads; after N connections the server drains remaining workers.
- On SIGINT/SIGTERM the main thread calls `shutdown()` on each active client
  while holding the mutex, joins them all, and restores its signal handlers.

## Phase 9 - Automatic leader election (experimental)

Run one process in each terminal (start all three for a full-cluster demo):

```sh
./build/raft-engine --elect config/node1.conf
./build/raft-engine --elect config/node2.conf
./build/raft-engine --elect config/node3.conf
```

Query any node from another terminal:

```sh
./build/raft-engine --status config/node1.conf 1
./build/raft-engine --status config/node1.conf 2
./build/raft-engine --status config/node1.conf 3
```

Expected format: `Status: node=1 role=leader term=1 voted_for=1`;
node IDs, roles and terms depend on race timing. The new `--elect` mode runs
randomized election timers, candidate transitions, self-voting, one-round
RequestVote requests and majority counting. `--serve` remains the Phase 8
passive RPC mode for backward-compatible tests.

**Not yet stable or production safe:** no AppendEntries heartbeats (Phase 10),
no durable term/vote state (later phase), no replicated log entries, and no
peer authentication or access control. Nodes may repeatedly elect new leaders
without heartbeats. `--status` is a debug-only unauthenticated RPC. A node
running alone must never call itself leader in a three-node cluster.

Run `make test` to exercise pure election transitions and the end-to-end
one-node/no-quorum, two-node/quorum and three-node status checks.

## Phase 10: stable leadership with AppendEntries heartbeats

Build and verify the complete project:

```sh
make clean && make && make test
./build/test_raft_append
python3 tests/test_heartbeat_integration.py ./build/raft-engine
```

Start three nodes in separate terminals:

```sh
./build/raft-engine --elect config/node1.conf
./build/raft-engine --elect config/node2.conf
./build/raft-engine --elect config/node3.conf
```

In a fourth terminal, inspect local states several times:

```sh
for i in 1 2 3; do ./build/raft-engine --status config/node1.conf "$i"; done
sleep 2
for i in 1 2 3; do ./build/raft-engine --status config/node1.conf "$i"; done
```

Under normal localhost conditions the same leader and term should appear
across successive samples. Heartbeats are empty AppendEntries RPC type 7
(36-byte payload), answered with type 8 (9-byte payload). Leaders send every
~90 ms. Recipients recognize a configured peer and reject stale terms; valid
leader contact resets election timeouts even when the previous-log consistency
check fails. The network integration test also kills the leader, verifies a
new leader with two survivors, then restarts the stopped process.

**Limitations:** There is no log-entry replication, crash-safe term/vote storage,
or state-machine application yet. The heartbeat sender currently polls peers
sequentially and uses short socket timeouts. Lack of reachable quorum does
not automatically demote a leader (Raft's basic model does not require that).
The wire protocol identifies senders only by fields in payloads; without
peer authentication, an untrusted client could impersonate a configured
node. Do not expose the educational server on the public network.

The private Phase 10 learning handbook is intentionally not included in the
repository; `.gitignore` excludes `*.pdf` and `learning-guides/`.

## Phase 11: follower log consistency and suffix repair (memory only)

This milestone adds `raft_log_matches`, `raft_log_last_term`, and
`raft_log_reconcile` to `include/raft_log.h`, alongside earlier append,
term lookup and truncation. `raft_log_reconcile` is the in-memory primitive
for Phase 12's future entry-carrying AppendEntries messages.

- A `(prev_log_index, prev_log_term)` pair must match before any changes.
- Index zero represents the virtual pre-log entry `(0,0)`.
- Entries that match by index and term are **not** duplicated or replaced.
- The first mismatching entry causes a replacement of the uncommitted suffix.
- Entries at or below the passed `committed_index` cannot be overwritten.
- Empty and fully matching batches do **not** erase an existing local tail.
- Input is checked and new payloads are deep-copied *before* the old
  log is changed. A validation/allocation failure leaves the log unchanged.
- These functions are **not thread-safe**; the future RPC handler must hold
  the node mutex, check term/membership, and durably persist accepted entries.

Build, run the demonstration, and test:

```sh
make clean && make
./build/raft-engine --log-demo
./build/test_raft_reconcile
sh tests/test_log_demo.sh ./build/raft-engine
make test
```

Expected demonstration summary:

```text
Log repair: matched_through=3 total_entries=3
entry[1]: term=1 command=SET x 1
entry[2]: term=3 command=SET y 2
entry[3]: term=3 command=SET z 3
In-memory demonstration only: no TCP log replication or disk writes.
```

To debug: `gdb --args ./build/raft-engine --log-demo` then use
`break raft_log_reconcile`, `run`, and `next`. For memory checks build with
`-fsanitize=address,undefined` as in the earlier Make/diagnostics section.

**Phase boundary:** The network's 36-byte AppendEntries request is still
heartbeat-only; no C cluster command replication, state machine application,
commit-majority logic, storage persistence or crash recovery exists yet.
Matching by `(index, term)` relies on Raft's leader append-only invariant:
correct leaders do not produce different commands at the same index and term.

**Repository hygiene:** Learning PDFs are delivered separately; `.gitignore`
excludes `*.pdf` and `learning-guides/`.

## Phase 12: AppendEntries entry replication over TCP

Build and run all tests:

```sh
make clean && make && make test
./build/test_raft_replication
python3 tests/test_replication_integration.py ./build/raft-engine
```

In Terminal A, start a standalone receiver (NOT an election cluster):

```sh
./build/raft-engine --serve config/node2.conf
```

In Terminal B, send a diagnostic AppendEntries command from configured node 1:

```sh
./build/raft-engine --append-demo config/node1.conf 2 3 0 0 "SET x 1"
./build/raft-engine --log-status config/node1.conf 2
./build/raft-engine --append-demo config/node1.conf 2 4 1 3 "SET y 2"
./build/raft-engine --log-status config/node1.conf 2
```

`--append-demo CONFIG PEER TERM PREV_INDEX PREV_TERM COMMAND` transmits
**one entry** (term = TERM) and prints the follower response. Test harnesses
also exercise multi-entry binary batches. `--log-status` returns last index,
last term and follower commit index; it does not expose command values.

These are developer-only probes: they do not consult the actual leader role,
update `nextIndex`/`matchIndex`, or prove a majority has received entries.
Use a fresh `--serve` receiver when repeating the demo; memory-only logs are
lost on restart. `--elect` remains the heartbeat/election test mode.

**Wire extension:** 36-byte AppendEntries header, followed (if nonempty) by a
2-byte big-endian count and up to 16 entries (`u64 term`, `u16 length`, raw
command bytes). The existing 36-byte empty heartbeat remains valid. All
messages are capped at 4096 bytes. Incorrect lengths, unknown IDs, stale terms,
missing previous entries and committed conflicts are rejected.

**Phase 13:** follower replication progress (`nextIndex`, `matchIndex`), retry
logic and eventual majority acknowledgement tracking.

## Phase 13: automatic leader replication and follower catch-up

In `--elect` mode, the leader now tracks **per-follower** `nextIndex` and
`matchIndex` and repeatedly sends bounded batches of actual log entries.
Failed prefix matches decrease `nextIndex` for the next attempt; network
failures leave the retry position unchanged. Higher-term replies cause
step-down. Batches still use the Phase 12 AppendEntries format (up to 16 entries,
4096 bytes) and empty batches remain ordinary heartbeats.

To create an entry on the **currently elected leader**, use this educational
CLI in a fourth terminal, choosing another node's config as the source:

```sh
# Terminal 1, 2, 3 (run concurrently):
./build/raft-engine --elect config/node1.conf
./build/raft-engine --elect config/node2.conf
./build/raft-engine --elect config/node3.conf

# Observe roles, then replace 2 with the actual leader ID:
./build/raft-engine --status config/node1.conf 2
./build/raft-engine --propose-demo config/node1.conf 2 "SET alpha 1"
./build/raft-engine --log-status config/node1.conf 2
./build/raft-engine --log-status config/node1.conf 3

# New tests:
./build/test_raft_progress
python3 tests/test_progress_integration.py ./build/raft-engine
```

`--propose-demo` is a **loopback-only, unauthenticated development command**.
It accepts 1–1024 bytes and rejects nonleaders. Success means appended to the
leader's *in-memory, uncommitted* log. It does **not** acknowledge majority
replication, commit, persistence, or state-machine execution. Do not expose
this listener on untrusted networks or use it as a production API.

The Phase 13 integration test starts three real servers on random loopback
ports, waits for election, appends entries, stops and restarts a follower,
checks automatic catch-up, and asserts `commitIndex` remains zero. The
stronger legacy log/node checks include an invalid-role `"unknown"` contract.

See `docs/architecture.md` and the separate Phase 13 learning PDF. Next:
**Phase 14 - majority commit tracking**, including the current-term Raft commit
rule and explicit separation of acknowledged replication from committed data.


## Phase 14: majority-backed commitIndex (fixed three-node cluster)

The elected leader now distinguishes **stored**, **replicated**, and **committed**
entries. `--propose-demo` still immediately reports admission to the leader's
memory (its `(uncommitted)` label describes that immediate response, **not** the
later cluster-wide commit outcome). After a same-term follower AppendEntries
success, `raft_commit_advance()` considers the leader's own last log index and
each follower's `matchIndex`. In our three-voter cluster, the *second highest*
index is the highest replicated on a majority (two nodes).

Crucial Raft rule: the leader advances `commitIndex` **directly only for entries
from its current term**. Once such an entry reaches a majority, every preceding
entry (including older-term entries) becomes committed indirectly. No quorum,
no new commit. On the next heartbeat, the leader advertises the updated
`leaderCommit`; followers cap their local `commitIndex` to the prefix they have
actually matched. The leader does not set `lastApplied` yet.

```
make clean && make && make test
./build/test_raft_commit
python3 tests/test_commit_integration.py ./build/raft-engine

# Start each node separately in three terminals:
./build/raft-engine --elect config/node1.conf
./build/raft-engine --elect config/node2.conf
./build/raft-engine --elect config/node3.conf

# Check roles, then use the actual leader ID (example: 2):
./build/raft-engine --status config/node1.conf 2
./build/raft-engine --propose-demo config/node1.conf 2 "SET alpha 1"
./build/raft-engine --log-status config/node1.conf 2
./build/raft-engine --log-status config/node1.conf 3
```

A successful diagnostic proposal is **not a client acknowledgment of commit**.
Poll `--log-status` to see `commit_index` progress. The Phase 14 integration test
starts three TCP processes on temporary loopback ports and verifies: majority
commit, propagation to followers, two-node quorum with one node down, no
additional commitment without quorum, and catch-up after rejoining.

**Important limitations:** Everything remains in memory. Restarts lose logs,
terms, votes and commit progress; this is **not crash-safe Raft**. In
particular, never infer durability from a majority of volatile copies.
The development proposal RPC is unauthenticated and must not be exposed outside
a trusted local lab. No key-value state-machine application (`lastApplied`
remains zero), snapshots, read consistency, storage layer, production client
protocol, or generalized membership. These are later milestones. See
`docs/architecture.md` and the separate Phase 14 learning guide.
