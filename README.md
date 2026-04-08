# Raft Engine

A learning-oriented distributed key-value store under construction in **C11**
on **Fedora Linux**, using **GNU Make** and a fixed three-node configuration.

**Current milestone: Phase 8 / 35 - RequestVote RPC.**
The application exchanges bounded, binary-safe **PING/PONG** and **RequestVote**
messages over TCP using eight bounded worker threads. RequestVote follows
term, one-vote-per-term, and log freshness rules **while the process is alive**.
**This is not yet a complete Raft election:** there are no background
candidate transitions, quorum counts, leader heartbeats, durable votes,
replicated consensus, or authenticated peer identities. Never expose this
educational transport to an untrusted network.

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

`make test` runs Phase 1-7 suites. Loopback tests select temporary endpoints,
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
