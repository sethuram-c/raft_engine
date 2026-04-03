# Raft Engine architecture (through Phase 3)

This page intentionally reflects implemented code, not aspirational features.

## Module dependencies

```text
                     src/main.c
                     /   |    \
           raft_node  raft_log  raft_config
               |         |          |
               +---------+----------+
                         |
                    raft_types.h
```

`raft_types.h` defines common ID/index/term/state types. `raft_log` owns a
resizable sequence of binary command buffers. `raft_node` owns a log and tracks
follower/candidate/leader state plus term, vote, commit and apply indices.
`raft_config` parses, validates and sorts an immutable value-type set of three
peer endpoints. It does **not** own external allocations.

## Configuration design

- Three independent files (`node1.conf`, `node2.conf`, `node3.conf`) all include
  the same static membership list, and each selects a different `node_id`.
- All addresses are IPv4 literals; no DNS, IPv6 or external libraries.
- All nodes use loopback interfaces for local development.
- A config is parsed into a zeroed temporary stack object; only after all
  checks pass is it assigned to the caller's output (failure atomicity).
- Numeric parsing checks both syntax and range instead of using `atoi`.
- `inet_pton(AF_INET)` validates IP addresses; comparisons use numeric IPv4.
- `peer` records are sorted for stable output; future transport may use IDs
  rather than relying on file order.

## Invariants

1. `node_id > 0` and appears in the peer list.
2. Exactly three unique nonzero peer IDs.
3. Every `ipv4` is syntactically valid and every `port` is in `[1, 65535]`.
4. No repeated numeric IPv4 address and port pair.
5. Successful parse produces deterministic order; on failure output unchanged.

## Next architectural layer

Phase 4 will consume this configuration to create TCP listening sockets and
accept peers. That implementation must handle partial reads, disconnections,
timeouts, bind failures and safe shutdown. No network behavior exists now.
