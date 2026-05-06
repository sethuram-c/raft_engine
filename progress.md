# Development notes

The repository was developed in small increments. This is a compact map of those increments, rather than a second copy of the README. The Git history and test files contain the implementation detail.

| Phases | Work completed |
| --- | --- |
| 1–3 | C11 build, node/log types, three-node configuration parsing |
| 4–7 | TCP listener, framed messages, bounded pthread workers, randomized election timers |
| 8–10 | RequestVote, election rounds, leader heartbeats |
| 11–12 | Log matching, conflicting suffix replacement, AppendEntries data over TCP |
| 13–15 | Per-follower replication progress, current-term majority commit, ordered SET/DEL application |
| 16–18 | Atomic state files, append-only WAL, journal checkpoints |
| 19–21 | State-machine snapshots, chunked InstallSnapshot, snapshot-aware WAL recovery |
| 22–24 | Commit-aware writes, request-ID deduplication, leader discovery and client retries |
| 25–27 | HMAC client authentication, mutual TLS, live certificate reload and transport deadlines |
| 28–30 | Metrics, readiness checks, controlled partition injection |
| 31–33 | Seeded chaos schedules, benchmark tooling, safer geometric log growth |
| 34–35 | Deterministic source releases, integrated TLS/auth/WAL acceptance test |

## Decisions worth remembering

**Do not equate replication with commitment.** The log might contain an entry on one or more nodes without that entry being committed. Even when a later leader has a majority of an earlier-term entry, it cannot directly advance commitment using that old-term entry alone.

**Timeout does not mean rollback.** The client must keep its request identity and payload when retrying an uncertain operation. That identity belongs in replicated state and snapshots, not in a leader-local cache.

**Snapshots change log indexing.** Removing the prefix must not renumber retained Raft entries. Snapshot index/term metadata and the suffix must recover as a consistent generation.

**Correctness tests should accept multiple valid schedules.** A follower restart can trigger a different election than expected. Test safety and eventual convergence rather than requiring a particular node to remain leader.

**Performance results need a narrow claim.** The geometric-growth optimization improved a synthetic append loop substantially; it did not multiply end-to-end client throughput by the same factor.

## Reproducing the checks

```sh
make clean && make && make test
make test-capstone
make test-chaos
make test-bench
make release && make test-release
make package-source && make verify-source
```

The complete gate takes time, especially on busy hosts. When an integration fails, reproduce that test alone first, then inspect logs and rerun the full suite. Process-kill recovery tests are not substitutes for power-loss tests.

The outstanding work is tracked in [docs/final-capstone.md](docs/final-capstone.md). The current code organization is in [docs/architecture.md](docs/architecture.md).
