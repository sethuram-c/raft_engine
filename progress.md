# Development progress

## Phase 1 / 35 - Fedora setup and GNU Make

**Implemented:** project skeleton, C11 executable, Git-ready layout, smoke tests.

## Phase 2 / 35 - Core Raft types and in-memory log

**Implemented:** node lifecycle, state/term/index types, binary-safe dynamic log,
local invariants, nine unit tests.

## Phase 3 / 35 - Static three-node configuration

**Implemented:** strict fixed-three-node parser, IPv4 endpoint validation,
example configuration files, CLI output and negative tests.

## Phase 4 / 35 - TCP server and peer connections

**Time budget:** ~2 hours
**Planned milestone:** January 4, 2027 (record actual commit time)

### Deliverable

A runnable TCP server that listens on the local configured IPv4:port, accepts
connections, logs them, cleans up file descriptors and shuts down gracefully;
plus a bounded-time peer connection probe and real loopback integration tests.

### Acceptance criteria

- [x] Listener binds configured local node endpoint
- [x] Listener accepts and closes real TCP sockets
- [x] `--probe` selects a configured peer ID and connects to it
- [x] `--probe` has bounded connection timeout and useful errors
- [x] Listener handles `SIGINT` and `SIGTERM` without unsafe signal work
- [x] Second listener on occupied endpoint is rejected
- [x] Invalid peer IDs and connection-count arguments are rejected
- [x] `make test` includes prior and new tests
- [x] Network tests use temporary ports (do not assume 7001-7003 are free)
- [ ] Run the complete acceptance tests on your own Fedora system
- [ ] Commit work after manual review

### Verify

```sh
make clean && make && make test
make check-config
./build/raft-engine --serve config/node1.conf
# In another terminal:
./build/raft-engine --probe config/node2.conf 1
```

### Commit message

```text
feat: add TCP listeners and peer connectivity probes
```

**Scope limit:** The accept loop closes each accepted descriptor immediately.
Raft RPCs and framed messages belong to later phases. No consensus yet.
