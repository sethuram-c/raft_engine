# Raft Engine

A learning-oriented C11 Raft/key-value engine for Fedora Linux.

**Current milestone: Phase 32 / 35 — Performance Profiling & Bottleneck Analysis.**

Phase 32 introduces `tools/raft_bench.py` for measured, majority-committed write
latency, throughput, Linux resource sampling, Raft metrics and failover timing.
See the Phase 32 benchmarking lab at the bottom of this README.

**Previous milestone: Phase 31 / 35 — Seeded Chaos Workloads & Repeatable Failure Schedules.**

Phase 31 builds a **rootless seeded stress harness** on Phase 30's test-only
`--elect-fault` server. It starts three localhost nodes, submits multiple
concurrent `SET` commands and a committed `DEL`, isolates the observed leader,
verifies no-quorum timeout, checks majority-side reelection and commitment,
heals the partition, then verifies committed key-value state on every node.
Some seeded rounds restart a follower and require it to catch up. A JSON action
plan can be saved and replayed; a JSONL event trace explains what actually
happened. Random seeds repeat **actions**, not the OS thread schedule or exact
Raft election timing.

```sh
make clean && make && make test
make test-chaos
python3 tools/raft_chaos.py --binary ./build/raft-engine \
  --seed 31 --rounds 2 --plan-out /tmp/raft31-plan.json \
  --trace /tmp/raft31-events.jsonl
python3 tools/raft_chaos.py --binary ./build/raft-engine \
  --replay-plan /tmp/raft31-plan.json --trace /tmp/raft31-replay.jsonl
```

Tests and artifacts are under `tests/` and `tools/`; no C consensus, wire,
storage, or TLS behavior changed in Phase 31. The harness is educational and
must be run only in an isolated local development environment. See the
**Phase 31 chaos lab** at the end of this README.

---

**Previous milestone: Phase 30 / 35 — Controlled Fault Injection & Recovery Validation.**

Phase 30 introduces a ROOTLESS **test-only** `--elect-fault CONFIG PRIVATE_RULE_DIR`
mode. Write per-node `node-ID.block` files to drop outbound internal Raft
RequestVote, AppendEntries, and InstallSnapshot traffic to the listed peer IDs.
Replace the policy files atomically to model directed partitions and heal them.
The new integration test proves majority-side re-election and commitment,
no-quorum write timeout, conflict rollback, state convergence, and restarted
follower recovery. Neither storage nor wire formats are changed. Older server
modes ignore these policy files and preserve their existing behavior.

```sh
make clean && make && make test
./build/test_raft_fault
python3 tests/test_fault_integration.py ./build/raft-engine
```

See the Phase 30 lab near the bottom of the README. **Do not expose this
fault-enabled mode outside an isolated development lab.**

---

**Earlier milestone: Phase 26 / 35 — Mutual TLS Transport & Raft Peer Identity Verification.**

Phase 26 adds **mutual TLS 1.3** for peer Raft RPCs and HMAC-authenticated
client writes. Each Raft node must present the exact certificate DNS SAN
`raft-node-ID`. A verified CA chain alone is not enough: incoming votes,
AppendEntries and snapshots must claim the authenticated sender identity.
Plain TCP remains only for prior development modes.

### Phase 26 quickstart

```sh
sudo dnf install gcc make openssl-devel openssl python3
make clean && make && make test
python3 tests/test_tls_integration.py ./build/raft-engine
```

For a disposable three-node TLS lab, `tests/test_tls_integration.py`
**automatically provisions a throwaway CA and certificates**, starts three
persistent TLS nodes, executes authenticated writes and checks rejected
spoofed identities and crash recovery. The full manual certificate procedure
and CLI syntax appear in the Phase 26 section near the end of this README.
Do not commit key files or expose the educational cluster on a public network.

---

**Earlier milestone: Phase 24 / 35 — Automatic Leader Discovery & Safe Client Retries.**

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

## Phase 25: per-client authenticated write admission (opt-in)

Phase 25 adds HMAC-SHA-256 signed retryable client writes without modifying
Raft's replication algorithm. Unlike `--write-auto`, the new command proves
possession of a **client-specific 32-byte secret**. The server looks up keys by
client ID in an owner-only keyring and verifies the MAC *before* checking
leadership, consulting deduplication records, or appending an entry. It rejects
legacy write/diagnostic mutation RPCs while running in authenticated mode.

The MAC signs `"RAFT25-CLIENT-WRITE-V1" || (timeout:u32 BE || client:u64 BE ||
request:u64 BE || SET/DEL bytes)`. On wire, it uses the existing Phase 23
request followed by a 32-byte HMAC-SHA-256 tag (frame types 25/26). The
response is the usual 17-byte Phase 22 status record. HMAC verification uses
OpenSSL and a constant-time MAC comparison. A credential is never transmitted,
printed in stdout, or passed as a command-line value.

Install prerequisites on Fedora: `sudo dnf install gcc make openssl-devel
openssl python3`. To configure local development credentials:

```sh
umask 077
mkdir -p /tmp/raft25-keys
openssl rand -hex 32 > /tmp/raft25-keys/client-71001.hex
# Provision the same client secret in the server keyring. A real deployment
# distributes the server keyring separately and NEVER ships it to clients.
printf '71001 %s\n' "$(cat /tmp/raft25-keys/client-71001.hex)" > /tmp/raft25-keys/server.keys
chmod 600 /tmp/raft25-keys/client-71001.hex /tmp/raft25-keys/server.keys
```

Run all three nodes with the same keyring (three terminals):

```sh
./build/raft-engine --elect-auth config/node1.conf /tmp/raft25-keys/server.keys
./build/raft-engine --elect-auth config/node2.conf /tmp/raft25-keys/server.keys
./build/raft-engine --elect-auth config/node3.conf /tmp/raft25-keys/server.keys
```

Or use **coordinated snapshot-WAL persistence** in an isolated directory:

```sh
./build/raft-engine --elect-auth-wal config/node1.conf data-auth /tmp/raft25-keys/server.keys
# Use matching node2.conf/node3.conf in the other terminals.
```

Clients need **only their own private secret file**:

```sh
./build/raft-engine --write-auth-auto config/node1.conf 8000 71001 1 \
  /tmp/raft25-keys/client-71001.hex 'SET user authenticated'
# Repeat the identical client ID, request ID and command after UNKNOWN.
```

`--write-id`, `--write`, `--write-auto`, `--propose-demo`, and
`--snapshot-compact-demo` are not accepted by a server in authenticated mode.
Existing non-authenticated modes deliberately remain compatible with their
previous, insecure behavior; **do not mix secured and unsecured nodes**.
All credential files must be regular files owned by the effective user, mode
0600 or stricter, and must not be symlinks. Keyring format: one
`CLIENT_ID 64_HEX_DIGITS` line per client, maximum 32 unique IDs.
Client secret format: one `64_HEX_DIGITS` line. No sample real key is included.

**Security boundary:** This is **not cluster security**. Raft peer RPCs,
status/read responses and the original transport are NOT authenticated or
encrypted; on a hostile network a peer-ID-spoofing attacker can still damage
consensus state. A client MAC can be replayed (replicated request IDs prevent
reapplying a recorded operation), and responses are not cryptographically
protected. Use loopback or a trusted isolated network for this educational
implementation. Future milestones should add mutual TLS for peer and client
channels, authenticated responses, secret rotation and rate limiting.

Run the new tests:

```sh
make clean && make && make test
./build/test_raft_auth
python3 tests/test_auth_integration.py ./build/raft-engine
```


## Phase 26: mutual TLS 1.3 peer/client transport (opt-in)

Phase 26 introduces an opt-in transport boundary that encrypts the **same
framed Raft protocol** using OpenSSL TLS 1.3. The original plaintext commands
remain available for backward compatibility, **but do not mix plaintext and
TLS nodes within one cluster**. TLS protects both requests and replies from
network observers; TLS mutual certificate authentication and SAN identity
checks protect against unauthenticated RPC senders.

**Secure durable server:**

```sh
./build/raft-engine --elect-tls-auth-wal CONFIG DATA_DIR KEYRING NODE_CERT NODE_KEY CA_CERT
```

**Secure authenticated client:**

```sh
./build/raft-engine --write-tls-auth-auto CONFIG BUDGET_MS CLIENT_ID REQUEST_ID \
  CLIENT_SECRET CLIENT_CERT CLIENT_KEY CA_CERT 'SET key value'
```

**Authenticated status inspection:**

```sh
./build/raft-engine --status-tls CONFIG PEER_ID CLIENT_CERT CLIENT_KEY CA_CERT
```

In the TLS-enabled server mode:

1. Each node presents an X.509 certificate with the exact DNS Subject
   Alternative Name `raft-node-N`, where N is its configured Raft ID.
2. Every TCP dial to a Raft member verifies the trust chain and the
   **expected** `raft-node-N` certificate identity (no wildcard/subject-CN
   fallback). The server verifies the sender's certificate with the same CA.
3. Incoming RequestVote, AppendEntries and InstallSnapshot RPCs may only
   claim a node ID equal to that in the client's authenticated certificate.
4. Client leaf certificates must carry SAN `raft-client`; a valid CA-signed
   certificate with an unrelated SAN is rejected. Phase 25 HMAC still signs
   each write and binds the claimed client ID, request ID and command.
5. Peer certificate and client certificate private keys **never** belong in
   the source repository or this ZIP. Restrict key access and rotate them
   deliberately if compromised.

A TLS-only socket will not understand the legacy plaintext `--status` or
`--write-auth-auto`; use the `*-tls-*` commands for TLS-enabled servers.
The 4096-byte message-frame limit and the Raft wire schema are unchanged.

### Generate throwaway development certificates

See the complete `tests/test_tls_integration.py` example, which uses OpenSSL
to make a disposable CA, three node certificates and a client certificate
with constrained SANs in a temporary directory. For a manual lab, generate
certificates signed by **one test CA** with these exact DNS SAN values:

| Credential | Required SAN | Used by |
|---|---|---|
| node1.crt | `DNS:raft-node-1` | `--elect-tls-auth-wal config/node1.conf` |
| node2.crt | `DNS:raft-node-2` | `--elect-tls-auth-wal config/node2.conf` |
| node3.crt | `DNS:raft-node-3` | `--elect-tls-auth-wal config/node3.conf` |
| client.crt | `DNS:raft-client` | `--status-tls`, `--write-tls-auth-auto` |

Server/client leaf certificates must include `extendedKeyUsage=serverAuth,clientAuth`
for this educational **shared-role** context, `basicConstraints=CA:FALSE`,
and a private key matching the certificate. Phase 25 owner-only HMAC keyring
and secret-file checks remain enabled. The test suite automatically generates
all material and removes it after the tests.

### Fedora checks

```sh
sudo dnf install gcc make openssl-devel openssl python3
make clean && make && make test
python3 tests/test_tls_integration.py ./build/raft-engine
```

The Phase 26 test exercises a genuine three-process election, TLS client
reads, HMAC-authenticated writes/retries, TLS certificate validation, denial
of plaintext access, mismatched local-node identity, a trusted node trying
to forge another Raft candidate ID, and snapshot-WAL `SIGKILL` recovery.
The previous phases' complete unit and network tests are retained.

### Security boundaries and known limitations

**This is a development transport, not a production security system.** There
is no certificate revocation (CRL/OCSP), automatic certificate issuance,
IP allow-list, or connection-rate limiting. Phase 27 adds manual SIGHUP
leaf/CA reload and an overall TLS handshake deadline, but not automatic renewal, client-ID
to certificate-SAN provisioning, or an external key-management service. The
CA is the cluster trust anchor: protect its private key offline; any entity
with that key can issue trusted certificates. The secure mode still exposes
read-only non-linearizable diagnostic RPCs to authenticated clients, while
its HMAC write path remains leader- and quorum-gated. TLS protects packets
*in transit*, not persisted files or a compromised host. No production
availability or power-loss behavior is claimed.

### Phase 26 staging list (10 files)

```sh
git add Makefile README.md progress.md docs/architecture.md \
  include/raft_tls.h src/raft_tls.c src/raft_tcp.c src/raft_wire.c \
  src/main.c tests/test_tls_integration.py
git diff --cached --check
git diff --cached --stat
git status
git commit -m "feat: protect raft peer and client RPCs with mutual TLS"
```


## Phase 27 — Certificate reload and TLS connection hardening

**No new server command is needed:** continue running `--elect-tls-auth-wal`
with the certificate, private key and CA file paths from Phase 26. Each
secure server now installs a SIGHUP handler *after it is ready*. Replace a
node's leaf certificate and private key at their original paths using atomic
file replacement, then reload without stopping Raft or deleting any WAL data:

```sh
# Replace both PEM files using your trusted, out-of-band provisioning tool.
# Certificate MUST retain DNS SAN raft-node-1 for node 1, and match its key.
chmod 600 /secure/path/node1.key
kill -HUP NODE1_PID
# Inspect the node's stderr for TLS credentials reloaded / reload rejected.
```

The process continues using its last successful `SSL_CTX` when the incoming
certificate, key or CA fails to load, the key has group/other permissions, or
its DNS SAN no longer matches the configured node ID. The server loads new
credentials *before* atomically swapping the pointer; existing TLS sessions
retain their old context until they finish. A replacement can temporarily
pair a new certificate with an old key during provisioning: such reloads
simply fail, leaving the old context available. Use atomic `rename(2)` for
**each** credential file, verify both are installed, and only then SIGHUP.

The key file must be a regular file owned by the process effective UID,
with no group/other bits and no symlink. All TLS sockets use TLS 1.3 with
session tickets disabled; an overall **1200 ms inbound** or **450 ms outbound**
handshake deadline bounds stalled handshakes regardless of socket idle
traffic. Workers remain bounded at **8** with a **16-connection** listening
backlog. In TLS-enabled processes, SIGPIPE is ignored so disconnected peers
cannot terminate the Raft node during OpenSSL writes. Plain TCP remains
compatible with all earlier modes and tests.

**CA rotation:** Use a bundle containing both old and new roots during a
planned overlap period, reload participating nodes and clients, replace
certificates, then remove the retired root from all bundles. Reloading one
node with a new CA only can temporarily break cluster quorum. These are
manual operator steps, not a coordinated trust-rollover protocol. Already
established sessions are not forcibly disconnected when a CA or certificate
is replaced; revocation, KMS/HSM protection, rate limiting, identity
provisioning and production-grade DoS resistance are still out of scope.

### Phase 27 Fedora verification

```sh
sudo dnf install gcc make openssl-devel openssl python3
make clean && make && make test
python3 tests/test_tls_lifecycle.py ./build/raft-engine
python3 tests/test_tls_integration.py ./build/raft-engine
```

The new test generates a disposable cluster CA, a renewed node certificate,
and a rejected same-CA certificate with a wrong SAN. It verifies SIGHUP
reload, fail-closed reload, restrictive key permissions, eight unfinished
handshakes, quorum-backed writes, and request-ID deduplication. The preceding
Phase 26 test is retained.

### Phase 27 complete Git staging list (9 files)

```sh
git add Makefile README.md progress.md docs/architecture.md \
  include/raft_tls.h src/raft_tls.c src/raft_tcp.c src/main.c \
  tests/test_tls_lifecycle.py
git diff --cached --check
git diff --cached --stat
git status
git commit -m "feat: rotate Raft TLS credentials and bound handshake resources"
```

**Next:** Phase 28 — operational observability, metrics and diagnostics.

## Phase 28 — Raft Observability, Metrics and Operational Diagnostics

Phase 28 adds a read-only operational metrics RPC to all existing server modes.
It does not change election/consensus rules, disk formats, HMACs, or the
previous `--status`, `--log-status` and `--apply-status` commands. The fixed
schema is version 1; counters are atomic and **reset when a process restarts**.
Node state gauges are taken under the existing node mutex.

```sh
make clean && make && make test
./build/raft-engine --elect config/node1.conf    # terminal 1
./build/raft-engine --elect config/node2.conf    # terminal 2
./build/raft-engine --elect config/node3.conf    # terminal 3
./build/raft-engine --metrics config/node1.conf 1  # fourth terminal
./build/raft-engine --metrics config/node1.conf 2
./build/raft-engine --metrics config/node1.conf 3
```

The CLI prints one stable `raft_<metric> <unsigned-integer>` line per field.
Important gauges: `role` (0 follower, 1 candidate, 2 leader), `current_term`,
`last_log_index`, `commit_index`, `last_applied`, `snapshot_index`,
`max_follower_lag` (leader-only maximum `lastLogIndex - matchIndex`).
Important counters: `election_rounds_total`, `leader_transitions_total`,
`vote_requests_total`, `votes_granted_total`, `append_requests_total`,
`append_rejections_total`, `replication_rpc_failures_total`,
`replication_rpc_successes_total`, `entries_sent_total`, `client_commits_total`,
`client_timeouts_total`, `snapshots_installed_total`,
`tls_handshake_failures_total`, `tls_reload_successes_total`,
`tls_reload_failures_total`, `connections_accepted_total`,
`connections_active`, `connections_peak`, `rejected_frames_total`, and
`storage_failures_total`.

For the TLS server from Phase 26/27, query with an **authorized raft-client mTLS
certificate** (or an authorized Raft node certificate):

```sh
./build/raft-engine --metrics-tls config/node1.conf 1 \
  /secure/raft/client.crt /secure/raft/client.key /secure/raft/ca-bundle.crt
```

A plaintext request to an mTLS server fails. In plaintext development modes,
metrics themselves are **unauthenticated** and must not be made reachable from
untrusted networks. Export no commands, credential bytes, certificate
contents, HMAC identities, or per-client data. Metrics use **no HTTP endpoint**
or Prometheus daemon; the CLI is a read-only snapshot over the existing Raft
framed TCP protocol (request type 27, response 28). The `RMET` payload is an
8-byte schema header followed by 29 big-endian 64-bit integers (240 bytes).
Unknown protocol versions/counts and invalid index invariants are rejected.

These counters are diagnostic, **not durable consensus state**. A commit
acknowledgement counter counts successful client responses (including same-ID
retry responses), not necessarily distinct committed operations. RPC success
means a well-formed response was received, not necessarily that the follower
accepted the log prefix. `rejected_frames_total` includes framing and generic
unsupported requests, but is **not** a complete audit log of every rejected
Raft semantic request. A TLS handshake failure is counted on the inbound
worker; a reload failure is counted only while the process remains running.
The gauges are sampled under one node lock but event counters can change
concurrently; scraping nodes does not yield a globally atomic cluster view.

Focused Phase 28 tests:

```sh
./build/test_raft_metrics
python3 tests/test_metrics_integration.py ./build/raft-engine
```

The integration test exercises leader election, majority-committed writes,
one offline follower with elevated lag and failed RPC counts, invalid metrics
frames, worker pressure, mTLS metrics access and rejection of plaintext/wrong
certificate identities. Operational counters are intentionally bounded and
fixed-schema; future versions should offer a carefully authenticated scrape
endpoint, sampling/histograms, and alert rules without holding consensus locks
across I/O.

### Phase 28 complete Git staging list (12 files)

```sh
git add Makefile README.md progress.md docs/architecture.md \
  include/raft_metrics.h include/raft_wire.h \
  src/raft_metrics.c src/raft_tcp.c src/raft_wire.c src/main.c \
  tests/test_raft_metrics.c tests/test_metrics_integration.py
git diff --cached --check
git diff --cached --stat
git status
git commit -m "feat: expose bounded raft metrics and operational diagnostics"
```

**Next:** Phase 29 — Health checks and safer operational readiness signals.

## Phase 29 — Operational Liveness and Write Readiness

Phase 29 adds a **non-mutating, versioned health RPC** to all server modes. It
answers two different questions: is the node responding to requests
(**liveness**), and is the node presently a leader with evidence of recent
same-term quorum contact (**operational write readiness**)? A successful
health RPC is proof of liveness for that single request; it is not proof that
the cluster can currently commit a new write. The `--ready` command compares
health replies from all configured nodes and deliberately refuses ambiguous
or conflicting leader reports.

```sh
make clean && make && make test
./build/test_raft_health
python3 tests/test_health_integration.py ./build/raft-engine

# With the three --elect nodes running, any terminal:
./build/raft-engine --health config/node1.conf 1
./build/raft-engine --health config/node1.conf 2
./build/raft-engine --health config/node1.conf 3
./build/raft-engine --ready config/node1.conf
```

An accessible follower reports `live=1 write_ready=0`. A leader reports
`write_ready=1` only while (1) automatic elections are enabled, (2) the node
has not entered fail-stop storage shutdown, and (3) at least one other voting
peer has **successfully acknowledged** same-term AppendEntries (or snapshot
transfer) within the last **3,000 monotonic milliseconds**. For this fixed
three-node configuration, leader plus one other voter is a majority. The
observation expires if both peers become unavailable; an isolated leader can
remain **live** while its readiness becomes **false**. This is a heuristic:
readiness is NOT a lease, a linearizable read guarantee, a durable health
record, a verified current quorum, or an authorization to bypass normal Raft
write acknowledgements. Failure of the health probe is also not a proof that
Raft has permanently failed.

The CLI returns exit code **0** for a successful `--health` request even if
`write_ready=0`; exit **1** means unreachable, unauthenticated or invalid
response. `--ready` returns **0** only when exactly one reachable node
reports recent-quorum write readiness, no other reachable node claims to be a
leader, and no reachable node reports a higher term; it returns **2** for an
unready/ambiguous cluster, **1** for invalid configuration. A missing follower
can be tolerated; readiness is intentionally conservative when reports
conflict. All probes use finite connection and socket timeouts.

The new wire types are 29 (`HEALTH_REQUEST`, **empty body**) and 30
(`HEALTH_RESPONSE`, **52 bytes**). The body is a big-endian, version-1 `RHLT`
record containing node ID, role, flags, term, commit index, applied index,
quorum-ack age and uptime. `quorum_age_ms=unknown` means no **fresh**
same-term acknowledgement. No secrets, commands, client IDs or TLS material
are sent. Invalid schemas and impossible flag combinations are rejected.

For mutual TLS servers, use authorized client credentials:

```sh
./build/raft-engine --health-tls config/node1.conf 1 \
  /secure/raft/client.crt /secure/raft/client.key /secure/raft/ca-bundle.crt
./build/raft-engine --ready-tls config/node1.conf \
  /secure/raft/client.crt /secure/raft/client.key /secure/raft/ca-bundle.crt
```

As before, plaintext development modes expose read-only diagnostics without
authentication and must be isolated from untrusted networks. TLS endpoints
require an authorized certificate and reject plaintext. The new health path
changes neither disk formats nor the Raft consensus protocol beyond adding
read-only RPC types. No HTTP server, Kubernetes probe endpoint, or production
health controller is added in this phase.

### Phase 29 exact Git staging list (12 files)

```sh
git add Makefile README.md progress.md docs/architecture.md \
  include/raft_health.h include/raft_wire.h \
  src/raft_health.c src/raft_tcp.c src/raft_wire.c src/main.c \
  tests/test_raft_health.c tests/test_health_integration.py
git diff --cached --check
git diff --cached --stat
git status
git commit -m "feat: expose raft liveness and conservative write readiness checks"
```

**Next:** Phase 30 — Fault injection, partition simulations and recovery validation.


## Phase 30 / 35 — Reproducible Rootless Raft Network Partitions

### Why fault injection is opt-in

The network partition lab tests **outbound Raft peer traffic only**, without
requiring root privileges, `iptables`, `tc`, or invasive changes to the Fedora
host network. Existing `--elect`, `--elect-snapshot-wal`, TLS modes, and their
compatibility tests are unaffected. The three-node lab uses **memory-only**
`--elect-fault`. It does not claim persistent recovery across machine crashes.

### Launch nodes in three terminals

First build and create a PRIVATE fault directory:

```sh
make clean && make
umask 077
mkdir -p /tmp/raft-phase30-faults
chmod 700 /tmp/raft-phase30-faults
./build/raft-engine --elect-fault config/node1.conf /tmp/raft-phase30-faults
# Separate terminals (use the same directory):
./build/raft-engine --elect-fault config/node2.conf /tmp/raft-phase30-faults
./build/raft-engine --elect-fault config/node3.conf /tmp/raft-phase30-faults
```

Find the currently elected leader with `--health`/`--status` and verify it can
commit a client write. If node 2 is the leader, isolate node 2 in **both
directions**. Write the policy with a same-directory temporary file and rename
so the election thread never reads a partial update:

```sh
printf '1\n3\n' > /tmp/raft-phase30-faults/.node-2.pending
mv /tmp/raft-phase30-faults/.node-2.pending /tmp/raft-phase30-faults/node-2.block
printf '2\n' > /tmp/raft-phase30-faults/.node-1.pending
mv /tmp/raft-phase30-faults/.node-1.pending /tmp/raft-phase30-faults/node-1.block
printf '2\n' > /tmp/raft-phase30-faults/.node-3.pending
mv /tmp/raft-phase30-faults/.node-3.pending /tmp/raft-phase30-faults/node-3.block
```

An isolated leader can remain TCP-live, but a new client write should TIMEOUT
rather than become COMMITTED. The connected pair should elect a higher-term
leader and commit independent entries. On healing, that higher term forces the
former leader to step down and reconcile its uncommitted conflicting suffix.

```sh
for n in 1 2 3; do
  : > /tmp/raft-phase30-faults/.node-$n.pending
  mv /tmp/raft-phase30-faults/.node-$n.pending /tmp/raft-phase30-faults/node-$n.block
done
```

The rule-file format is ASCII unsigned decimal peer IDs separated by spaces,
newlines, tabs, or carriage returns; absent file means no drops. Rule files
must be owner-owned regular files, not writable by group/others, not symlinks,
and at most 128 bytes. Malformed, unreadable, unsafe, or oversized files **drop
all outbound Raft RPCs** from that node (fail closed). The directory itself
must be owned by the current user with no group/other permissions. Only
`--elect-fault` uses these rules; they do NOT protect inbound peer RPCs, client
connections, diagnostics, or TLS traffic. For a bidirectional partition, set
policy rules on **both sides**.

### Verification

```sh
make test
./build/test_raft_fault
python3 tests/test_fault_integration.py ./build/raft-engine
```

The C test covers valid rules, changing rules while running, failure policy,
size limits, bad tokens, symlink rejection, group-writable files, and directory
permissions. The three-process test covers healthy majority commit, partition,
no-quorum timeout, re-election, majority-side commitment, healing and prefix
repair, full convergence, and a restarted in-memory follower catching up.

**Boundaries:** Faults are injected at the outgoing peer-RPC call site, not at
the kernel network layer. Delay, packet corruption, disk failures, one-way
responses, TCP handshakes, TLS behavior, and simultaneous power loss are NOT
simulated here. This is not a formal model check or a production network
partition facility.

### Phase 30 exact Git staging list (11 changed files)

```sh
git add Makefile README.md progress.md docs/architecture.md \
  include/raft_fault.h include/raft_tcp.h \
  src/raft_fault.c src/raft_tcp.c src/main.c \
  tests/test_raft_fault.c tests/test_fault_integration.py
# 11 paths above, including two tests.
git diff --cached --check
git diff --cached --stat
git status
git commit -m "test: add controlled raft partitions and recovery invariants"
```

**Next:** Phase 31 — Systematic stress workloads and repeatable chaos scenarios.


## Phase 31 chaos lab — seeded schedules and correctness oracles

The new `tools/raft_chaos.py` creates a validated, versioned JSON action plan
and optionally writes an ordered JSONL observation trace. The plan is portable:
no TCP ports, process IDs, wall-clock times or private data are embedded.
The harness always uses temporary loopback configurations and a private fault
rule directory; it leaves no firewall or persistent cluster state behind.

```sh
python3 tests/test_chaos_plan.py
python3 tests/test_chaos_integration.py ./build/raft-engine
python3 tools/raft_chaos.py --binary ./build/raft-engine --seed 41 --rounds 3 \
  --plan-out /tmp/raft-chaos-plan.json --trace /tmp/raft-chaos-events.jsonl
python3 tools/raft_chaos.py --binary ./build/raft-engine \
  --replay-plan /tmp/raft-chaos-plan.json --trace /tmp/raft-chaos-replay.jsonl
python3 tools/raft_chaos.py --seed 41 --rounds 3 --plan-only
```

A round performs (1) concurrent, independent committed SET commands, (2) a
committed DEL of a selected key, (3) a bidirectional minority partition,
(4) an isolated-leader write that must not be acknowledged or applied, (5) a
higher-term election and committed majority-side write, (6) healing and
state convergence, and (7) optionally restarting an in-memory follower.
The harness checks `commitIndex <= lastLogIndex`, samples leader roles by
term, and validates that every confirmed key/value or deletion survives
partition healing and catch-up. Local GET diagnostics are not linearizable.

`--seed` accepts unsigned 32-bit integers; `--rounds` accepts 1 through 8.
Plans are schema-validated before replay. A seed reproduces workloads and
fault-action order, **not** process scheduling, exact timings, or elected node
IDs. JSONL traces record live observations and can differ across replays.
The runner uses Phase 30's `--elect-fault` mode only; it never injects faults
into production-oriented server modes.

**Scope boundaries:** one three-voter cluster per run, localhost, outbound
Raft peer RPC drops, memory-only server state, bounded timeouts. This is
neither a Jepsen linearizability checker nor a formal proof of all Raft
interleavings; it does not cover disk faults, kernel-level delay/reordering,
TLS, security or real multi-machine deployments. Treat intermittent failures
as leads for investigation and preserve the seed/plan/trace and server logs.

### Phase 31 exact Git staging list (7 paths)

```sh
git add Makefile README.md progress.md docs/architecture.md \
  tools/raft_chaos.py tests/test_chaos_plan.py tests/test_chaos_integration.py
git diff --cached --check
git diff --cached --stat
git status
git commit -m "test: add seeded raft chaos workloads and replayable schedules"
```

---

## Phase 32 / 35 - Performance profiling and bottleneck diagnosis

Phase 32 adds an isolated, **measurement-only Python benchmark**. It launches
three temporary `--elect-fault` Raft nodes on localhost, times majority-committed
`--write` requests, and measures an actual majority-side election plus the
first committed write after the leader is partitioned. Each benchmark checks
that confirmed key-value data is visible across the cluster after healing.
No C consensus logic, wire format, WAL format, TLS configuration or legacy tests
were replaced.

```sh
make clean && make && make test
make test-bench
make bench

# Compare two levels of CLI-client concurrency, using similar hosts / load.
python3 tools/raft_bench.py --binary ./build/raft-engine \
  --writes 60 --concurrency 1 --warmup 6 --no-failover \
  --json-out /tmp/raft32-c1.json --csv-out /tmp/raft32-c1.csv
python3 tools/raft_bench.py --binary ./build/raft-engine \
  --writes 60 --concurrency 4 --warmup 6 \
  --json-out /tmp/raft32-c4.json --csv-out /tmp/raft32-c4.csv
```

**What is timed:** one entire external CLI invocation (process creation, TCP,
replication, commit acknowledgement), **not** an internal Raft function or
pure network latency. The measured interval starts when work is submitted to
the client executor and ends once every client finishes. Throughput is the
number of acknowledged **COMMITTED** requests divided by this interval.
Warmup writes are not counted. Latency quantiles use the **nearest-rank**
method; tiny sample sizes should not be used to claim accurate P99 latency.

The benchmark records per-write samples as CSV and aggregate results as JSON,
including Phase 28 Raft counter snapshots, sampled `/proc/PID` resident memory
and CPU, and partition-to-new-leader/confirmed-write timing. CPU sampling has
clock-tick granularity and may show zero under light loads; RSS values are
**sampled**, not exact process peaks. The fault creates a test-only directed
partition with rootless per-node rule files. Sampled observations are not
formal proof of performance or safety, and election timing varies across runs.

For repeatable comparisons: pin compiler/settings/hardware, avoid running
other tests, run multiple repetitions, increase samples, report distributions
and failed requests rather than only best-case latency. Do not compare
unauthenticated in-memory mode to durable TLS mode as if their costs were
equivalent. Results depend on kernel scheduling, TCP loopback, power/CPU
settings and measured host load. Never treat generated values as Fedora-wide
performance guarantees.

Tests:

```sh
python3 tests/test_bench_metrics.py
python3 tests/test_benchmark_integration.py ./build/raft-engine
```

### Phase 32 exact Git staging list (7 files)

```sh
git add Makefile README.md progress.md docs/architecture.md \
  tools/raft_bench.py tests/test_bench_metrics.py \
  tests/test_benchmark_integration.py
git diff --cached --check
git diff --cached --stat
git status
git commit -m "perf: add reproducible raft benchmarking and failover profiling"
```

**Next:** Phase 33 - targeted optimization and performance regressions.
