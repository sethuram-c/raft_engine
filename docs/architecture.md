# Raft Engine architecture (Phase 2)

This document describes **implemented code**, and distinguishes it from future
components. As of Phase 2, this is **one single-process, in-memory model**.

## Current components

```text
            src/main.c
                |
                v
           raft_node_t
       +-------------------+
       | id, role, term    |
       | voted_for         |
       | commit_index      |
       | last_applied      |
       | raft_log_t -------+---> entries[0..count-1]
       +-------------------+         | each command is owned
                                     | each entry has a term
```

- `raft_types.h`: fixed-width identities, term and index types, roles, errors.
- `raft_log.[ch]`: mutable, **volatile** log with dynamic storage. The real
  entry at index `k >= 1` is stored at `entries[k-1]`. Index 0 is virtual.
  `raft_log_append` deep-copies arbitrary bytes. `raft_log_truncate_from(k)`
  frees the suffix starting at index k. A later phase will implement durable
  storage and replication.
- `raft_node.[ch]`: in-memory state container with a basic invariant checker.
  `last_applied <= commit_index <= last_log_index` and local log terms cannot
  exceed currentTerm. It does **not** implement election/consensus transitions.
- `tests/`: unit tests validate ownership, indexing, validation, truncation,
  capacity growth, initial state, and invalid node progress bounds.

## Key invariants and caveats

1. Valid node IDs start at 1; ID 0 is the `RAFT_NO_VOTE` sentinel.
2. Every real entry has `term >= 1`; a missing virtual entry at index 0 has term 0.
3. New entries may have equal or larger terms, never smaller terms.
4. A caller still owns its input bytes; the log owns an independent copy.
5. Each log/node must be initialized before use and destroyed before disposal.
6. An entry pointer from `raft_log_at` can be invalidated by append or truncate.
7. `raft_node_is_valid` checks structural consistency only. It does **not**
   certify election safety, replication safety, or majority commitment.
8. No thread safety, network messages, or durability exists in this phase.

## Future phase boundaries

- Phase 3: configuration parsing and fixed-cluster settings.
- Phases 4-6: TCP framing, inter-node communication, worker synchronization.
- Phases 7-10: election timers, RequestVote, leadership, heartbeats.
- Phases 11-15: replicated entries and quorum-based commitment.
- Phases 19-21: durable term, vote, and log storage and restart recovery.

The final project targets a fixed three-node distributed key-value store,
not a production-ready consensus library. Dynamic membership, snapshots,
and full linearizable read coordination are out of scope.
