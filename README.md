# raft_engine

A small Raft implementation in C11 for Linux. It runs a fixed three-voter cluster, replicates a `SET`/`DEL` log over TCP, and applies committed entries to a key-value map. I built it to work through the engineering problems behind Raft: message framing, elections, conflicting logs, crash recovery, retries, and the operational problems that show up when nodes fail.

The repository is a **learning project, not a production database**. A passing test run does not establish linearizability under every schedule, disk-failure safety on every filesystem, or suitability for an untrusted deployment.

## Build

On Fedora:

```sh
sudo dnf install gcc make python3 openssl openssl-devel
make
make test
```

`make` produces `build/raft-engine` with debugging symbols. The tests include C unit tests and real three-process integration tests; the network tests take longer and can occasionally be sensitive to host load.

Useful additional targets:

```sh
make release          # optimized binary: build/raft-engine-release
make test-capstone     # integrated TLS/auth/durability scenario
make test-chaos        # seeded partition and recovery campaign
make test-bench        # performance-tool smoke/integration tests
make bench             # write throughput/latency experiment
make bench-reconcile   # isolated log append benchmark
make check-release     # extended release acceptance gate
```

For a shorter loop while changing an individual module, run its `build/test_raft_*` executable and then rerun `make test` before committing.

## Run three nodes locally

The examples use the fixed membership files under `config/` and localhost ports. Open three terminals:

```sh
./build/raft-engine --elect config/node1.conf
./build/raft-engine --elect config/node2.conf
./build/raft-engine --elect config/node3.conf
```

From a fourth terminal, inspect the election:

```sh
for id in 1 2 3; do
    ./build/raft-engine --status config/node1.conf "$id"
done
./build/raft-engine --ready config/node1.conf
```

The leader is whichever node currently reports `role=leader`; it is not necessarily node 1. For a commit-aware client write, supply the ID of that leader:

```sh
./build/raft-engine --write config/node1.conf LEADER_ID 2500 'SET city Fedora'
./build/raft-engine --get-demo config/node1.conf LEADER_ID city
```

`--write` reports `COMMITTED` only after majority commitment. `TIMEOUT` or `UNKNOWN` does **not** mean the write was rolled back. The GET command is a local diagnostic read, not a linearizable read API.

### Retryable writes

Use a stable client ID and monotonically increasing request ID with `--write-auto`:

```sh
./build/raft-engine --write-auto config/node1.conf 8000 71001 1 'SET city Fedora'
```

If the outcome is uncertain, retry with **the same IDs and identical command**. The client discovers the leader and the server deduplicates requests using replicated metadata. This implementation tracks a limited number of client sessions and expects sequential requests from each client; it is not a general-purpose exactly-once API.

## Storage and authenticated modes

The simple `--elect` mode is entirely in memory. Other modes are opt-in; they do not all share a disk format:

| Mode | Purpose |
| --- | --- |
| `--elect` | In-memory elections, replication and commits |
| `--elect-wal CONFIG DIR` | Incremental write-ahead logging |
| `--elect-snapshot CONFIG` | In-memory snapshot installation |
| `--elect-snapshot-wal CONFIG DIR` | Snapshot-aware WAL and restart recovery |
| `--elect-auth CONFIG KEYRING` | HMAC-authenticated client writes |
| `--elect-auth-wal CONFIG DIR KEYRING` | Authentication with snapshot-WAL storage |
| `--elect-tls-auth-wal` | Mutual TLS, authenticated writes and snapshot-WAL persistence |
| `--elect-fault CONFIG RULE_DIR` | Controlled outbound-RPC faults for local testing |

Check the detailed argument order using `./build/raft-engine --help` and the examples in `docs/final-capstone.md`. Do not point two voters at a copied node state, or reuse one directory across incompatible storage modes. Keep experimental data on local filesystems in private directories.

The strongest demonstration is the TLS/authenticated/snapshot-WAL capstone. It provisions throwaway test credentials and cluster data in temporary directories:

```sh
make test-capstone
make capstone-report
```

The second command creates `build/capstone-report.json` with test outcomes and a digest of the optimized executable. It does not include private keys or node logs.

## Code map

| Location | Main responsibility |
| --- | --- |
| `src/raft_election.c`, `src/raft_vote.c` | Election timeouts and RequestVote |
| `src/raft_log.c`, `src/raft_append.c` | Log storage, matching and conflict repair |
| `src/raft_replication.c`, `src/raft_commit.c` | Follower progress and majority commit |
| `src/raft_state_machine.c`, `src/raft_dedup.c` | Ordered application and request deduplication |
| `src/raft_snapshot.c`, `src/raft_install.c` | Snapshot data and InstallSnapshot transfers |
| `src/raft_wal.c`, `src/raft_snapwal.c` | WAL/checkpoint persistence and recovery |
| `src/raft_tcp.c`, `src/raft_wire.c` | Connection workers and bounded wire framing |
| `src/raft_tls.c`, `src/raft_auth.c` | Mutual TLS and client request authentication |
| `src/raft_health.c`, `src/raft_metrics.c` | Read-only operational signals |
| `tests/` | Unit and network integration tests |
| `tools/` | Chaos, benchmarking, packaging and capstone runners |

For data flow, synchronization boundaries, and protocol details, see [docs/architecture.md](docs/architecture.md). For setup and safe operational experiments, see [docs/release-operations.md](docs/release-operations.md). The phase-by-phase work log is in [progress.md](progress.md).

## Tests worth running while developing

```sh
./build/test_raft_log
./build/test_raft_reconcile
./build/test_raft_commit
./build/test_raft_snapshot
python3 tests/test_fault_integration.py ./build/raft-engine
python3 tests/test_capstone_integration.py ./build/raft-engine
```

The partition tests run disposable three-node clusters and can fail on a heavily loaded machine due to election timing. A repeatable chaos schedule is useful for investigating a failure:

```sh
python3 tools/raft_chaos.py --binary ./build/raft-engine \
  --seed 31 --rounds 2 --plan-out /tmp/raft-plan.json \
  --trace /tmp/raft-trace.jsonl
```

A saved plan repeats the fault/workload **actions**, not kernel scheduling or exact election timing. Do not interpret a green chaos campaign as an exhaustive Raft safety proof.

## Release and current limits

```sh
make package-source
make verify-source
```

The source packager generates a deterministic ZIP and an internal SHA-256 manifest. The manifest detects accidental changes; it does not authenticate who published the archive. Generated files under `build/` and `dist/` should stay out of Git.

Known limits that matter before anyone deploys this beyond a lab:

- Cluster membership is fixed at three voters. There is no joint-consensus reconfiguration.
- GET is diagnostic and lacks a linearizable read protocol.
- Client session tracking is bounded and retains only the latest request per client.
- Secret distribution, certificate revocation, and upgrades are not fully designed.
- Power-loss recovery has not been validated across filesystems and storage hardware.
- The fault harness simulates selected outgoing RPC failures, not all packet or disk faults.
- There is no formal model-checking or independent security audit.

See [docs/final-capstone.md](docs/final-capstone.md) for the integrated acceptance scenario and the remaining engineering work.
