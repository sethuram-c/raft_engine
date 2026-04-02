# Development progress

## Phase 2 / 35 — Core Raft types, log, and node model

**Budget:** approximately 2 hours (theoretical study can take longer).

### Deliverable

A compilable, testable, in-memory Raft domain model with deep-copy command
storage, log indexing and truncation, and a node invariant checker. No
networking or consensus behavior is claimed.

### Acceptance criteria

- [ ] `make clean && make` builds warning-free using GCC and C11.
- [ ] `make demo` shows a follower and two sample log entries.
- [ ] `make test` passes 4 smoke checks and 9 C unit-test functions.
- [ ] Append operations preserve binary command bytes after caller mutation.
- [ ] Invalid indices and nonmonotonic appended terms are rejected.
- [ ] Truncation frees discarded command buffers and supports re-append.
- [ ] Invalid node commit/applied bounds fail invariant checks.
- [ ] Phase 2 changes committed and pushed after review.

### Commands

```sh
make clean && make
make demo
make test
valgrind --leak-check=full --error-exitcode=1 ./build/test_raft_log
valgrind --leak-check=full --error-exitcode=1 ./build/test_raft_node
```

### Main files changed

- Makefile, README.md, progress.md, docs/architecture.md
- src/main.c, src/raft_log.c, src/raft_node.c
- include/raft_types.h, include/raft_log.h, include/raft_node.h
- tests/test_smoke.sh, tests/test_raft_log.c, tests/test_raft_node.c

### Commit subject

```text
feat: implement in-memory raft node and log data structures
```

The day-two planning slot is January 2, 2027, after 20:30 IST. The date should
only be recorded in Git if it reflects when the work was actually completed.

## Phase 1 / 35 — Project bootstrap

Established the Fedora C11 + GNU Make project, Git workflow, a command-line
executable, and automated smoke testing.
