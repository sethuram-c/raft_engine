# Raft Engine architecture (through Phase 4)

This document describes the implemented system, not future guarantees.

## Implemented modules

```text
                      src/main.c
                     /    |    \
           raft_node/  raft_config  raft_tcp
                |          |          |
             raft_log      |          +-- raft_config
                |          |                |
                +----------+----------------+
                           |
                      raft_types.h
```

- `raft_types.h`: C types for node identifiers, terms, indices, and roles.
- `raft_log.c`: heap-owning in-memory log with append, lookup and truncation.
- `raft_node.c`: local node initialization and invariant checks.
- `raft_config.c`: static, validated, sorted three-peer IPv4 configuration.
- `raft_tcp.c`: sequential TCP listener, connection-only probe and shutdown.
- `main.c`: command-line interface for demonstrations, config, server and probe.

## Phase 4 TCP listener lifecycle

```text
config file -> validate -> select local endpoint
                                |
       socket(AF_INET, SOCK_STREAM, 0)
                                |
                         SO_REUSEADDR
                                |
                        bind(ip, port)
                                |
                         listen(backlog)
                                |
                       poll(POLLIN, 250ms) <--- SIGINT/SIGTERM sets stop flag
                                |
                            accept()
                                |
                      log remote IP:port
                                |
                           close(client)
                                |
                         next connection
```

The listener is sequential and closes each accepted connection immediately.
**There is no framed protocol yet**. Probe code performs a nonblocking TCP
connect bounded by a timeout; it checks `SO_ERROR` after `poll(POLLOUT)`.

## File descriptor ownership

- `raft_tcp_listen` and `raft_tcp_connect` return owned descriptors on success.
- Their callers must close them exactly once.
- The server owns its listener and every accepted client descriptor.
- Every failure after `socket()` closes that descriptor before returning.
- `sigaction` handlers change only a `volatile sig_atomic_t` flag.
- `poll()` checks the flag periodically, avoiding unsafe operations in handlers.

## Current network trust boundaries

- The listener accepts any TCP client reachable at its IPv4 endpoint.
- Source port/IP printed by `accept` is **not** authenticated Raft node identity.
- A successful `connect` proves only TCP reachability, not a healthy Raft node.
- No TLS, message encoding, election timers or replicated writes exist.
- Do not expose this learning demo to untrusted public networks.

## Configuration invariants (from Phase 3)

- `node_id` is nonzero and appears among exactly three peers.
- Each peer has a unique positive ID and a unique IPv4:port endpoint.
- All addresses are IPv4 literals and all ports are between 1 and 65535.
- The config parser leaves its output unchanged on invalid input.
- Peer order is sorted by ascending ID for deterministic output.

## Next steps

Phase 5 will add length-prefixed message framing and a defined wire protocol.
Subsequent phases will implement threading, Raft RPCs, timeouts and leader
election. No Raft agreement should be inferred from Phase 4 TCP connections.
