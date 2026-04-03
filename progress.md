# Development progress

## Phase 1 / 35 — Fedora setup + GNU Make

**Implemented:** project skeleton, executable, Git-ready layout, smoke tests.

## Phase 2 / 35 — Core Raft types and in-memory log

**Implemented:** Raft IDs and roles, node lifecycle, binary-safe dynamic log,
local invariants, smoke tests and 9 unit tests.

## Phase 3 / 35 — Static three-node configuration

**Target time:** ~2 hours  
**Planned milestone date:** January 3, 2027 (record genuine commit times)

### Deliverable

A CLI that reads a human-editable three-node static cluster file,
validates all peers, and prints a deterministic cluster summary.

### Acceptance criteria

- [x] C11 implementation and public config API
- [x] Three example loopback configuration files
- [x] Local ID exists in the exactly-three-peer list
- [x] Duplicate peer IDs and duplicate IPv4:port addresses rejected
- [x] Malformed, overflowing, zero and out-of-range numbers rejected
- [x] Unknown keys, too-long lines and extra peers rejected
- [x] Output destination unchanged if parsing/validation fails
- [x] `make test` passes including earlier log/node tests
- [x] `make check-config` succeeds for all three nodes
- [ ] Run on your own Fedora machine
- [ ] Review and commit the change to your own Git repository

### Verify

```sh
make clean && make && make test
make check-config
./build/raft-engine --check-config config/node2.conf
```

### Commit suggestion

```text
feat: validate fixed three-node raft cluster configuration
```

**Scope limit:** this milestone does not start three processes, open TCP
listeners, establish consensus, or persist Raft state. Networking is next.
