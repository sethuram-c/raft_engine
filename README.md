# Raft Engine

An educational, fixed-membership distributed key-value store written in **C11**.
This project is built incrementally around the Raft consensus algorithm using
**GNU Make** on **Fedora Linux**.

**Current status: Phase 2 / 35.** Core Raft node and log **data structures** are
implemented and tested. Networking, leader election, actual replication,
persistence, and quorum-based commitment are **not yet implemented**.

## Implemented in Phase 2

- Typed node IDs, terms, log indices, roles, and return codes.
- In-memory Raft log with 1-based indices and an index-0 virtual prefix.
- Deep-copied binary command buffers; geometric dynamic-array growth.
- Log term lookup and suffix truncation, including edge-case validation.
- Raft node initialization, destruction, and structural invariant checks.
- Two C unit-test executables plus CLI smoke tests.
- A `--demo` command displaying a synthetic node with sample log entries.

## Planned features

- Three-node leader election, heartbeats, and TCP RPCs.
- Conflict resolution, replicated logs, and majority-based commits.
- Deterministic key-value state machine and client interface.
- Persistent term/vote/log storage and crash recovery.
- Failure-injection and integration testing.

## Requirements

Fedora Linux, GCC, GNU Make; GDB and Valgrind are recommended:

```sh
sudo dnf install -y gcc make git gdb valgrind
```

## Build and verify

```sh
make clean
make              # Builds build/raft-engine
make run          # Prints startup message
make demo         # Shows sample in-memory Raft node and log
make test         # Runs CLI smoke checks AND C unit tests
make test-unit    # Runs the log and node unit tests only
make test-smoke   # Runs CLI tests only
make clean        # Deletes generated build/ files
```

`make test` should end with:

```text
PASS: 5 Raft log unit tests
PASS: 4 Raft node unit tests
```

The smoke script additionally checks version, startup, demonstration output, and
invalid CLI arguments. The version is `raft-engine 0.2.0`.

### Optional memory checks

```sh
make test
valgrind --leak-check=full --error-exitcode=1 ./build/test_raft_log
valgrind --leak-check=full --error-exitcode=1 ./build/test_raft_node
```

## Repository structure

```text
raft-engine/
├── Makefile
├── include/
│   ├── raft_types.h      # Primitive IDs, roles, status codes
│   ├── raft_log.h        # In-memory log API and ownership contract
│   └── raft_node.h       # In-memory node data model
├── src/
│   ├── main.c            # CLI and synthetic demonstration
│   ├── raft_log.c        # Grow, copy, lookup, truncate, free
│   └── raft_node.c       # Node initialization and local invariants
├── tests/
│   ├── test_smoke.sh     # CLI smoke checks
│   ├── test_raft_log.c   # Log unit tests
│   └── test_raft_node.c  # Node unit tests
├── docs/architecture.md
├── progress.md
├── .gitignore
└── README.md
```

## Design notes

Real log entries use **indices 1, 2, 3, ...**. The conceptual entry at index
0 has term 0 but is not stored. Commands are binary-safe, individually owned
by the log, and freed on truncation or destruction. Logs remain volatile until
persistent storage is added in a later phase. Direct field modifications in
the demo and tests do **not** imply implemented Raft elections or commitment.

This is a learning project, not production-ready infrastructure. Scope excludes
snapshots, dynamic membership, and fully linearizable reads.

**Learning materials:** Per-phase PDFs are distributed separately and must not
be committed. `.gitignore` excludes `*.pdf` and `learning-guides/`.
