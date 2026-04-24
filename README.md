# Raft Engine

A learning-oriented C11 Raft/key-value engine for Fedora Linux.

**Current milestone: Phase 24 / 35 — Automatic Leader Discovery & Safe Client Retries.**

Phase 24 adds a topology-aware, opt-in `--write-auto` CLI. It discovers
leaders using the **existing 17-byte STATUS RPC**, selects a unique leader
at the highest observed term, and sends the Phase 23 request-ID write RPC.
It automatically retries **exactly the same encoded request** on leader
changes, transient errors, and uncertain timeouts, bounded by an overall
monotonic-clock deadline. STATUS is only a hint: every write is validated
again by the contacted server. The configuration's `node_id` identifies the
config file, not the client, and may point at any of the three running nodes.

### Phase 24 quickstart

```sh
make clean && make && make test
./build/test_raft_client_auto
python3 tests/test_auto_client_integration.py ./build/raft-engine

# Start the three nodes in separate terminals using --elect or
# --elect-snapshot-wal CONFIG FRESH_DATA_DIRECTORY.
./build/raft-engine --write-auto config/node1.conf 8000 91001 1 'SET order paid'
# Reuse exactly the SAME identity and command if the entire CLI times out.
./build/raft-engine --write-auto config/node1.conf 8000 91001 1 'SET order paid'
# Only advance the request ID after the result is resolved.
./build/raft-engine --write-auto config/node1.conf 8000 91001 2 'DEL order'
```

`--write-auto CONFIG OVERALL_TIMEOUT_MS CLIENT_ID REQUEST_ID COMMAND` uses an
overall deadline of **500–30,000 ms**. It does not require a leader ID. Output
includes discovery, final status, attempted writes and discovery rounds.
Exit codes are `0=COMMITTED`, `3=UNKNOWN/indeterminate`, `4=REJECTED`,
`1=invalid arguments/runtime error`. Node unavailability is retried until the
budget expires. An uncertain result must be retried only with identical IDs
and bytes, including across separate CLI invocations. The server still
restricts clients to one outstanding request per client ID and at most 32
active client IDs; it does not authenticate them.

**Boundaries:** This is a best-effort bounded client loop, not a lease or
proof of unique global leadership. Each TCP operation has a shorter timeout
than the overall budget. The status protocol and legacy `--write`, `--write-id`
behaviors remain unchanged. There is no authenticated client identity,
linearizable read, automatic disk persistence of client counters, or promise
that `COMMITTED` means the state-machine operation succeeded. No additional
wire protocol types are needed for Phase 24.

### Phase 24 Git staging list

```sh
git add Makefile README.md progress.md docs/architecture.md \
  include/raft_client_auto.h src/raft_client_auto.c src/main.c \
  tests/test_raft_client_auto.c tests/test_auto_client_integration.py
git diff --cached --check
git status
git commit -m "feat: discover raft leaders and safely retry client writes"
```

---

**Previous milestone: Phase 23**



Phase 23 extends Phase 22 with an **opt-in `--write-id` API**. The client
supplies nonzero 64-bit `(client_id, request_id)` values. The replicated log
contains the identity and exact command. Every node applies a given client
request at most once, and the applied-session table survives state-machine
snapshot compaction, snapshot installation, and snapshot-WAL crash recovery.

### Phase 23 quickstart

```sh
make clean && make && make test
./build/test_raft_dedup
python3 tests/test_dedup_integration.py ./build/raft-engine

# Start three nodes in three terminals (durable example uses a fresh folder):
./build/raft-engine --elect-snapshot-wal config/node1.conf data23
./build/raft-engine --elect-snapshot-wal config/node2.conf data23
./build/raft-engine --elect-snapshot-wal config/node3.conf data23

# Discover leader ID with --status; here assume node 2 is elected.
./build/raft-engine --write-id config/node1.conf 2 2500 81001 1 'SET order_1 placed'

# Repeat exactly the same IDs and command after an ambiguous timeout:
./build/raft-engine --write-id config/node1.conf 2 2500 81001 1 'SET order_1 placed'
# Move to request 2 ONLY after request 1 is committed or resolved:
./build/raft-engine --write-id config/node1.conf 2 2500 81001 2 'DEL order_1'
```

`--write-id CONFIG PEER_ID TIMEOUT_MS CLIENT_ID REQUEST_ID COMMAND` uses
strictly increasing request numbers per client and **one outstanding request
per client**. Reuse the same ID and the **identical raw command** on every
retry. Equal committed requests return the original `(term, index)` without
new admission. Conflicting reuse, stale numbers, or a different request while
one is pending return `REJECTED`. Timeouts and network errors remain
indeterminate; retrying the same identity is safe (do not silently switch
identities). Other-client progress is unaffected by an outstanding request.

**Scope and limits:** Deduplication covers only `--write-id` (not the legacy
`--write` or diagnostic proposal RPC). Up to **32 distinct client IDs** can be
tracked per state machine; records are never silently evicted. IDs are not
authenticated; this is a trusted laboratory API with no client-side ID
allocator or automatic redirection. A `COMMITTED` result confirms Raft
commitment, not guaranteed successful key insertion (table-full commands
remain deterministic no-ops). Client IDs and request numbers must be
persisted by the *client* across restarts. Snapshot version 2 is used when
session records exist; legacy version 1 snapshots remain decodable.

### Phase 23 test commands

```sh
make clean && make && make test
./build/test_raft_dedup
python3 tests/test_dedup_integration.py ./build/raft-engine
```

---

**Previously (Phase 22):** Phase 22 adds a bounded, leader-targeted **`--write` client operation** that returns
`COMMITTED` only after the leader's majority-backed `commitIndex` reaches the
new entry in the same leadership term. It accepts valid `SET` / `DEL`
commands, rejects follower writes and malformed commands, and distinguishes
`TIMEOUT` from `NOT_LEADER`. Timeouts are **indeterminate**: a timed-out
entry may still commit later; there is no request-id deduplication yet.

### Phase 22 quickstart

```sh
make clean && make && make test
./build/test_raft_client_write
python3 tests/test_client_write_integration.py ./build/raft-engine
python3 tests/test_client_write_durable.py ./build/raft-engine

# Start three nodes in separate terminals, with either --elect or
# --elect-snapshot-wal CONFIG DATA_DIR.
./build/raft-engine --elect config/node1.conf
./build/raft-engine --elect config/node2.conf
./build/raft-engine --elect config/node3.conf

# Discover the current leader with --status first. For example, if
# node 2 is leader, use node1.conf as the routing configuration:
./build/raft-engine --write config/node1.conf 2 2500 'SET alpha hello'
```

Exit codes: `0=COMMITTED`, `2=NOT_LEADER`, `3=TIMEOUT/UNKNOWN`,
`4=REJECTED`, `1=transport/protocol/CLI failure`. The `--write` command does
not retry or redirect automatically. A transport error can also conceal a
committed write. Status reports refer to **commit**, not necessarily local
state-machine application or a durable read. The server has an eight-worker
limit; long waits can occupy all worker slots for at most 3 seconds each.

**Security:** Client RPCs remain unauthenticated, and the CLI may not connect
using the target node's own identity configuration. Use a different configured
node's `.conf` to address the target. This is a trusted-lab interface, not a
production database client. State-machine table-full operations remain
committed no-ops; the response does not guarantee that a key was stored.

---

**Previously (Phase 21):** Phase 21 introduces a **separate, opt-in** `--elect-snapshot-wal` mode that
persists Raft consensus metadata, compacted snapshot bytes, and the retained
log suffix in **one atomic checkpoint generation**. Between generation
checkpoints, term/vote/commit and log-suffix changes are appended to the
checksummed journal and `fsync`ed before acknowledgment. Startup validates
and replays the checkpoint and journal, reconstructs the snapshot KV state,
then applies any committed entries after the compacted boundary. A completed
InstallSnapshot or leader compaction is acknowledged only after its combined
checkpoint is durable; storage failures cause a fail-stop.

### Phase 21 quickstart (fresh, separate directory)

```sh
make clean && make && make test
./build/test_raft_snapwal
python3 tests/test_snapwal_integration.py ./build/raft-engine

# Run each in a different terminal; do NOT mix with Phase 16-18 data.
./build/raft-engine --elect-snapshot-wal config/node1.conf data-phase21
./build/raft-engine --elect-snapshot-wal config/node2.conf data-phase21
./build/raft-engine --elect-snapshot-wal config/node3.conf data-phase21

# Discover the leader, then submit and commit proposals before compaction:
./build/raft-engine --status config/node1.conf 2
./build/raft-engine --propose-demo config/node1.conf 2 'SET alpha 1'
./build/raft-engine --snapshot-compact-demo config/node1.conf 2
```

Each identity uses `data-phase21/node-ID.snapwal` plus the shared node-ID
lock. These new files **cannot** be passed to `--elect-wal` or
`--elect-durable`, and old journals are not automatically migrated.

**Trust and limitations:** This is still an educational engine. Development
RPCs are unauthenticated and not suitable for an untrusted network; no
linearizable client API, log-install streaming to disk, power-cut simulation,
production durability audit, or generalized membership. CRC32 detects
accidental corruption but is not a MAC. Tested crashes use `SIGKILL` and
controlled on-disk corruption; real storage hardware guarantees vary.

Phase 20 introduces a separate, **in-memory only** `--elect-snapshot` cluster mode.
After quorum commitment and local application, a development-only administrative
RPC can compact the elected leader through `lastApplied`. A follower whose
replication position is at/before that boundary then receives a CRC32-checked
snapshot in bounded, offset-acknowledged framed TCP chunks. The follower
installs KV state and the virtual `(lastIncludedIndex, lastIncludedTerm)`
log boundary, preserves a matching suffix, and resumes AppendEntries.

### Phase 20 quickstart

```sh
make clean && make && make test
./build/test_raft_install
python3 tests/test_install_integration.py ./build/raft-engine

# Start a three-node cluster (separate terminals):
./build/raft-engine --elect-snapshot config/node1.conf
./build/raft-engine --elect-snapshot config/node2.conf
./build/raft-engine --elect-snapshot config/node3.conf

# Discover the leader (example: node 2):
./build/raft-engine --status config/node1.conf 2
./build/raft-engine --propose-demo config/node1.conf 2 'SET alpha 1'
# After the entry has committed and applied, trigger educational compaction:
./build/raft-engine --snapshot-compact-demo config/node1.conf 2
./build/raft-engine --apply-status config/node1.conf 2
```

**Safety boundary:** The existing `--elect`, `--elect-durable` and `--elect-wal`
modes are not changed to compact Raft prefixes or install snapshots. The
Phase 20 transfer is **not crash durable**, does **not** migrate the WAL format,
and is **not authenticated**. Do not use it with production data. Phase 21 will
focus on snapshot persistence / WAL-coordinated recovery.

---

**Previously (Phase 18): Phase 18 / 35 — Crash-atomic WAL checkpoints and compaction.**
The Phase 17 `--elect-wal` mode now periodically compacts its journal into one
**full-image checkpoint record inside the WAL file**, retaining the complete
Raft log (including uncommitted suffixes), term, vote and commit index. This
is **journal-history compaction**, not Raft log truncation or a state-machine
snapshot. The serialized record remains compatible with Phase 17's on-disk
format; existing journals load without migration.

### Phase 18 quickstart

```sh
make clean && make && make test
./build/test_raft_checkpoint
python3 tests/test_checkpoint_integration.py ./build/raft-engine

# Start the normal WAL cluster in three terminals:
./build/raft-engine --elect-wal config/node1.conf data
./build/raft-engine --elect-wal config/node2.conf data
./build/raft-engine --elect-wal config/node3.conf data

# Stop the cluster, then compact the three WALs OFFLINE:
./build/raft-engine --compact-wal config/node1.conf data
./build/raft-engine --compact-wal config/node2.conf data
./build/raft-engine --compact-wal config/node3.conf data
# Restart the WAL cluster using the SAME original data directory.
```

**Automatic policy:** after 64 durable record writes, or when the journal is
at least 256 KiB and contains at least 4 records, the currently loaded node
checkpoint is written and atomically installed. This bounds **number of
delta records** replayed between checkpoints, not the number of log entries.
The original in-memory log limits (4096 entries and max 4096 bytes per
command), and maximum WAL size (64 MiB), remain in effect. WAL compaction
retains the entire log and does NOT make log growth asymptotically bounded.
A future Raft snapshot/install-snapshot phase is needed for safe prefix
trimming and true long-term log-space bounds.

**Crash transition:** writer holds the node lock and serializes consensus
mutations. It encodes a self-contained checkpoint with sequence 1 and
`keep=0`, writes a `node-ID.wal.compact.tmp` file with mode 0600,
`fsync`s that file, `renameat`s it over `node-ID.wal`, then `fsync`s the
directory. Before rename the old WAL is authoritative; after rename the
checkpoint WAL is authoritative. Both images can be loaded independently,
so power failure at the rename boundary leaves either a valid old history or
its state-equivalent checkpoint on filesystems honoring those primitives.
If any durability operation fails, the caller must fail-stop. A stale temp
file from a pre-rename crash is discarded while holding the lock at startup.

**Offline command:** `--compact-wal CONFIG DATA_DIR` acquires the same
per-node exclusive lock as `--elect-wal`; it fails while that identity is
running. It loads and validates the complete existing WAL before touching
the live file. CRC-corrupt or semantically invalid complete records fail
closed; only Phase 17's incomplete final-record recovery rule still applies.
Do not use a wrong/new directory; the WAL open primitive creates an empty
journal if one is missing. Back up your data directory before experiments.

**Compatibility:** Phase 16 `--elect-durable` remains separate. Existing
Phase 17 `node-ID.wal` files remain readable; no migration of snapshot mode.
Unauthenticated diagnostic proposals still acknowledge admission, not commit;
local GET is not linearizable. These files and examples are for local
learning, not production or adversarial environments.

---

## Previous milestone — Phase 17 incremental WAL

**Current milestone: Phase 17 / 35 — Append-only Write-Ahead Log (WAL) and incremental replay.**
The new `--elect-wal` mode persists incremental changes to term, vote, commit
index, and changed log suffixes in checksummed, length-framed journal records.
The record is appended and `fsync`ed before a consensus acknowledgement.
After `SIGKILL`, startup replays the verified records in sequence, truncates
*only an incomplete final record* and rebuilds committed KV state. A complete
record with an invalid CRC, sequence, term, or log transition fails closed.

### Phase 17 quickstart

```sh
make clean && make && make test
# Three distinct terminals, SAME data directory, DIFFERENT node identities:
./build/raft-engine --elect-wal config/node1.conf data
./build/raft-engine --elect-wal config/node2.conf data
./build/raft-engine --elect-wal config/node3.conf data
# Find leader, then use its ID (e.g. 2) in local diagnostic commands:
./build/raft-engine --status config/node1.conf 2
./build/raft-engine --propose-demo config/node1.conf 2 'SET greeting hello'
./build/raft-engine --apply-status config/node1.conf 2
./build/raft-engine --get-demo config/node1.conf 2 greeting

# Focused Phase 17 tests:
./build/test_raft_wal
python3 tests/test_wal_integration.py ./build/raft-engine
```

**Disk layout:** `data/node-ID.wal` and `data/node-ID.lock`. The data
file begins with a 20-byte versioned/CRC-protected header and then append-only
records (8-byte length/CRC frame plus metadata/changed log suffix). Per-node
`flock` prevents two live writers. The journal is limited to 64 MiB, and the
in-memory log remains limited to 4096 entries / 4096 command bytes per entry.
There is **no checkpoint compaction yet**; reaching the WAL cap causes
fail-stop behavior. Metadata-only records are much smaller than Phase 16's
complete snapshot rewrites, but replay of long journals can be expensive.

**Compatibility boundary:** The prior `--elect-durable` mode still uses
`node-ID.raft` snapshots. Do not switch the same node/data directory between
snapshot and WAL modes: both explicitly reject a conflicting file. To learn
the WAL mode with existing Phase 16 data, start a distinct **new educational
cluster** using a different directory; do not erase a participating node's
state as a migration strategy. There is no automatic migration protocol.

**Recovery contract:** Only an interrupted/incomplete final WAL record is
truncated and followed by `fsync`. A fully present record with invalid CRC or
metadata prevents startup without resetting prior state. The returned node
starts as follower; `lastApplied` and KV state are reconstructed by replaying
committed log entries. Hardware durability depends on Linux and filesystem
fsync guarantees. CRC32 detects accidental corruption, not malicious edits.

**Important:** This remains an educational demo. The unauthenticated proposal
acknowledges *admission*, not Raft commit; diagnostic reads are not
linearizable. No WAL compaction, disk migration, authenticated RPCs, safe
membership changes, or safe node replacement yet. Never run on untrusted
networks.

---

## Previous milestone — Phase 16 durable snapshots

A learning-oriented C11 distributed key-value store for Fedora Linux.

**Current milestone: Phase 16 / 35 — Durable Raft term, vote, log and commit
metadata groundwork.** The new opt-in `--elect-durable` mode restores local
consensus state after process crash; `--elect` remains volatile. The disk image
is versioned, checksummed, size-bounded and replaced with a temp-file fsync,
atomic rename, and directory fsync. Each durable node holds an advisory
per-node storage lock. Restart always begins as follower and deterministically
replays the committed prefix into a new KV state machine.

### Phase 16 Fedora quickstart

```sh
make clean && make && make test
# In three separate terminals (same directory; different node IDs):
./build/raft-engine --elect-durable config/node1.conf data
./build/raft-engine --elect-durable config/node2.conf data
./build/raft-engine --elect-durable config/node3.conf data
# Identify the leader, then replace ID 2 below with that leader:
./build/raft-engine --status config/node1.conf 2
./build/raft-engine --propose-demo config/node1.conf 2 'SET greeting hello'
./build/raft-engine --get-demo config/node1.conf 2 greeting

# Focused durability tests:
./build/test_raft_storage
python3 tests/test_storage_integration.py ./build/raft-engine
```

**Warning:** The development-only proposal returns on admission, NOT on
commit. Diagnostic GET is local and not linearizable; neither RPC is
authenticated. The stored snapshot is a full log rewrite, NOT a WAL or a
production database. Only run on trusted loopback. Do not wipe and reuse a
previously participating node's durable data to simulate safe replacement.


### Durable mode: what is and is not guaranteed

- One node's storage files are `data/node-ID.raft` and `data/node-ID.lock`.
  `data/` is excluded by `.gitignore`; use a separate directory outside Git
  if desired. The directory is created with 0700 and new files with 0600.
- Durable state changes are written before granting a new vote, sending a
  self-vote request, acknowledging a new AppendEntries log, or acknowledging
  an admitted diagnostic proposal. Commit advancement is also saved.
- A full state image is serialized in network byte order (not raw C structs)
  with format version, node ID, term, vote, commit index, log entries and CRC32.
  Temporary-file `fsync`, rename, and directory `fsync` make each update
  crash-atomic on local filesystems that honor those durability operations.
- The reader rejects malformed/truncated/corrupt files and wrong node IDs; it
  does not silently bootstrap after such a failure. A per-node advisory lock
  rejects duplicate processes using the same data directory and node ID.
- Recovery always returns as a follower, resets the election deadline, and
  rebuilds the KV state deterministically from entries through `commitIndex`.
  `lastApplied` and the KV contents are reconstructed, not stored separately.
- Per-peer `nextIndex` / `matchIndex`, role, sockets and timer deadlines are
  volatile. There is no WAL, compaction, snapshot installation, authentication,
  configuration-change protocol, linearizable GET, or full crash-proof testing.
- Limits: 4 MiB snapshot, 4096 log entries, 4096 bytes per command. Once full,
  a durable write fails closed. Full-snapshot fsync is intentionally expensive.
- **Do not delete a node's durable file and restart that same voting identity.**
  This learning demo has no protocol for safe node replacement.



---

## Historical notes through Phase 15

A learning-oriented distributed key-value store under construction in **C11**
on **Fedora Linux**, using **GNU Make** and a fixed three-node configuration.

**Historical milestone: Phase 15 / 35 - Ordered in-memory state-machine application.**
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


## Phase 19: state-machine snapshots and in-memory prefix compaction

A versioned `RAFT19SN` snapshot saves the **applied** key-value state,
`lastIncludedIndex`, `lastIncludedTerm`, and deterministic execution counters.
Keys and values are binary-safe where appropriate. CRC32 rejects accidental
corruption. The offline demo uses temporary-file write, file `fsync`, atomic
rename and directory `fsync` before demonstrating an **in-memory-only** trim.
The compacted log keeps the virtual `(base_index, base_term)` entry, which
allows newer suffix entries to keep their original absolute Raft indices.

```sh
make clean && make && make test
./build/test_raft_snapshot
mkdir -p /tmp/raft-phase19-lab
./build/raft-engine --snapshot-demo /tmp/raft-phase19-lab
ls -l /tmp/raft-phase19-lab/phase19.snap
```

**Safety boundary:** `--elect-wal` and `--elect-durable` intentionally **do
not** compact their live logs or consume Phase 19 snapshot files. Their
persisted formats still lack a compacted-log base index/term and snapshot
generation transactions, and TCP has no InstallSnapshot RPC yet. The old disk
writers reject compacted logs rather than silently persisting incomplete
state. Phase 19 is a thoroughly tested *offline format and indexing milestone*,
not production-ready space reclamation. Do not run this demo against a live
node's data directory. Full log/WAL replacement and lagging-follower snapshot
installation remain future work. See `docs/architecture.md`.
