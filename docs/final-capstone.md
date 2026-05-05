# Raft Engine — Phase 35 Final Capstone & Engineering Review

**Status:** Educational 35-phase C11/Fedora Raft learning project. **Not production certified.**

This is a source-level capstone, not a product-security certification or an
assertion that running tests proves the full Raft algorithm. This project does
not claim to pass a formal Raft verification suite or Jepsen/linearizability
analysis. The capstone deliberately keeps the consensus implementation,
serialized network messages, WAL formats and snapshot encodings unchanged.

## 1. System architecture and project result

The engine is a statically configured three-node cluster with Raft roles,
randomized elections, leader heartbeats, bounded framed TCP messages,
RequestVote / AppendEntries / InstallSnapshot, and sequential application of
committed `SET`/`DEL` entries to an in-memory key-value state machine.
Snapshot-aware WAL persistence, client request-ID deduplication, automatic
leader discovery, optional HMAC write authentication, mutual TLS 1.3,
health/metrics RPCs, rootless partition faults, chaos testing and benchmarks
are educational modes/tools with documented limits.

`src/main.c` handles the CLI; `src/raft_tcp.c` manages the server workers and
synchronized node state; `src/raft_election.c`, `src/raft_append.c`,
`src/raft_replication.c`, `src/raft_commit.c`, and `src/raft_state_machine.c`
implement the core Raft pipeline. `src/raft_snapwal.c` stores snapshot-aware
state; `src/raft_auth.c` and `src/raft_tls.c` support the authenticated mode.
`tools/` contains the chaos/benchmark/release/capstone tooling.

### Stages across 35 phases

| Phases | Engineering topic | Evidence and notable limits |
|---|---|---|
| 1–7 | C11 foundations, config, TCP, framed protocol, threading, timers | unit tests, CLI smoke, TCP framing/concurrency |
| 8–10 | Elections and heartbeats | RequestVote, multi-node leader selection and heartbeat stability |
| 11–15 | Log matching, replication, commitment, application | conflict repair, matchIndex, current-term commit and deterministic KV |
| 16–21 | Durable snapshots, WAL, InstallSnapshot and coordinated recovery | corrupt/torn record tests, SIGKILL recovery; filesystem-dependent durability |
| 22–27 | Client writes, safe retry, HMAC, TLS, TLS lifecycle | authenticated writes and same-ID retry; no general-purpose PKI lifecycle |
| 28–30 | Metrics, readiness, fault injection | operational visibility; readiness not a linearizable/lease guarantee |
| 31–33 | Seeded chaos, benchmarks, measured optimization | synthetic workload tests, bounded measurements; not proof of safety |
| 34–35 | Reproducible source release, capstone audit | SHA256SUMS, release smoke, integrated secured crash/retry validation |

## 2. What the new capstone tests

`tests/test_capstone_integration.py` starts an isolated cluster in temporary
private directories with **unique certificate/private-key pairs** for three
nodes, a throwaway CA, an authenticated client certificate, and per-client
HMAC secrets. It runs the real executable, not an in-process Raft mock.

1. Start three snapshot-WAL/TLS/HMAC nodes on disposable localhost ports.
2. Observe one writable leader using the TLS status and readiness RPCs.
3. Reject a CA-signed but unapproved client SAN for a TLS health request.
4. Commit a signed client write and verify same-ID/same-command replay reports
   the original log index rather than admitting a second logical operation.
5. Reject conflicting reuse of an already-used client/request ID.
6. Inspect TLS metrics for log/commit/application ordering and bounded worker
   concurrency. These are *observations*, not linearizable proofs.
7. Kill one follower; demonstrate the surviving two voters can still commit
   a **new** authenticated write. Verify a node-specific WAL exists.
8. Restart the lagging follower; wait until it applies the committed entry.
9. `SIGKILL` all three, restart from the same per-node snapshot-WAL files, and
   wait for a new healthy leader.
10. Commit a **current-term barrier entry using a different client identity**
    before retrying the first client's latest previous request. Confirm the
    recovered result retains its original index.

The final step is intentional: a client session only retains its *latest*
request identity in this implementation. After issuing request 2, do not retry
request 1 and expect the old result. A new leader also must not advance its
commit index using only a majority-acknowledged older-term entry, so a
current-term barrier is used in the test.

This capstone is not designed to simulate a true power failure. It cannot
certify correct file-system flushes after power loss. Full fault coverage
remains an open engineering task.

## 3. Quickstart on Fedora

```sh
sudo dnf install gcc make python3 openssl openssl-devel
make clean
make
make test-capstone             # Debug-build integrated three-node scenario
make test                      # All phases' unit + network regression suites
make release                   # Hardened -O2 candidate
make capstone-report           # Three acceptance stages, JSON report
make capstone-extended         # Additional TLS, chaos and benchmark stages
make package-source
make verify-source
make check-release             # Long release + full regression + capstone gate
```

For direct control (run `make release` first):

```sh
python3 tools/raft_capstone.py \
  --binary ./build/raft-engine-release \
  --json-out build/my-capstone-report.json

# More extensive (takes longer and can be timing-sensitive under load):
python3 tools/raft_capstone.py \
  --binary ./build/raft-engine-release \
  --json-out build/my-capstone-extended.json --extended
```

The basic runner executes optimized CLI/config smoke tests, source-release
archive tests, and the new integrated TLS/durable/duplicate-retry scenario.
The extended runner adds older TLS integration, seeded partition/chaos, and
three-node benchmark validation.

The JSON report includes a hash of the compiled executable, Python version,
platform, UTC generation time, each stage's exit status, elapsed milliseconds,
and successful check count. It contains **no private keys, certificates,
HMAC secrets, cluster commands, raw stdout/stderr, or node logs**. A failing
stage stops the runner; an incomplete suite cannot report success.

`make capstone-report` needs no root, real certificates, or installed cluster:
all experiment credentials/config/data are ephemeral. It does require OpenSSL
CLI, OpenSSL development libraries, and local loopback sockets. The runner
per-stage timeout defaults to 180 seconds (`--timeout 20..600`).

## 4. Final acceptance and review checklist

**Automated, local acceptance**

- [ ] `make clean && make && make test` exits 0 on the learner's Fedora machine.
- [ ] `make release && make test-release` passes using the optimized binary.
- [ ] `make capstone-report` prints three passing stages and a `"passed": true`
      report at `build/capstone-report.json`.
- [ ] `make capstone-extended` additionally passes TLS, chaos and benchmark.
- [ ] `make package-source && make verify-source` validates every archive member.
- [ ] Compare the package's SHA-256 digest with a copy distributed separately
      through a trusted channel; the internal manifest is **not a signature**.
- [ ] Confirm `git diff --check` and `git status` show no unreviewed changes.

**Manual security and operations review (not automatically guaranteed)**

- [ ] Choose an explicit project redistribution license before publishing.
- [ ] Obtain independent security/code review and documented CVE response plan.
- [ ] Design tested certificate/key rotation, expiry, revocation and escrow.
- [ ] Implement genuine linearizable read semantics or document a weaker API.
- [ ] Design membership changes, upgrade compatibility and disk format migration.
- [ ] Validate partial writes, fsync semantics and real power-loss behavior.
- [ ] Review client session-table exhaustion and retries across long lifetimes.
- [ ] Review persistent snapshot transfers under all crash windows.
- [ ] Add structured trace-based linearizability checking, broad fuzzing and
      adversarial/network-kernel tests (tc/netem, partitions, clock skew).
- [ ] Harden transport resource/DoS boundaries and provision monitoring.

A passing acceptance suite does **not** check any of these unchecked items.

## 5. Known constraints and engineering decisions

- **Fixed membership:** three configured voters, no safe dynamic reconfiguration.
- **Reads:** the GET demo is a local diagnostic, not a linearizable read path.
- **Client identities:** retry tracking has bounded session capacity, requires
  sequential per-client IDs, and preserves only the most recent identity.
- **Security:** mutual TLS and HMAC operate in opt-in modes, but broader secret
  and certificate lifecycle and public-exposure hardening remain incomplete.
- **Durability:** snapshot-WAL recovery is exercised with process crashes;
  verified power-loss consistency across many filesystems is not established.
- **Faults:** the rootless fault engine blocks outgoing RPC classes, not real
  packets; it does not model disk corruption, timing and all network behaviors.
- **Benchmark:** Phase 33's ~241x microbenchmark result describes a specific
  isolated append path, not an end-to-end client throughput gain.
- **Provenance:** SHA-256 manifests detect changes; they do not authenticate a
  publisher. Release signing is not provided.

## 6. Engineering retrospective: patterns worth keeping

**Safety invariants beat happy-path checks.** Vote monotonicity, the
current-term commitment rule, committed-prefix protection, crash recovery,
client-session identity, and same-term acknowledgement handling need explicit
regression assertions. The project learned that a test asserting a particular
leader stays elected after a restart is too brittle; test safety, not a single
schedule.

**Small protocols need exact ownership rules.** A versioned frame, bounded
payloads and defensive decoding work hand-in-hand with binary-safe command
ownership and failure-atomic log replacement. Thread safety and lock lifetime
must be audited alongside TLS context swaps and shutdown paths.

**Separate physical durability from logical commitment.** A log can be saved,
replicated and still uncommitted. A committed entry can be awaiting application.
A snapshot can compress applied history while carrying its Raft boundary and
retaining a suffix. A new leader's older-term entries require the normal
current-term commitment mechanism.

**Security and operations are layered.** HMAC authenticates write identities;
mTLS authenticates transport peers and protects channel contents; metrics and
readiness indicate operational state. None of these by itself proves consensus
safety, full availability or production security.

**Reproducibility improves diagnosis.** Stable source archives, full Git
diffs, seed-recorded chaos schedules, explicit binaries/toolchain versions,
and machine-readable capstone reports make regressions actionable. Do not
backdate Git commits; record the actual day the work was completed.

## 7. Suggested final Git tag (after local review)

```sh
git status
make test && make check-release
git add Makefile README.md progress.md docs/architecture.md \
  docs/final-capstone.md tools/raft_capstone.py \
  tests/test_capstone_integration.py tests/test_capstone_report.py
git diff --cached --check
git diff --cached --stat
git commit -m "test: complete raft capstone verification and final engineering review"
git tag -a v0.1.0-edu -m "Raft Engine educational 35-phase capstone"
git status
```

Only create the tag after tests and manual review have passed on the learner's
machine. A Git tag is not a code-signing signature and does not address the
production blockers above.
