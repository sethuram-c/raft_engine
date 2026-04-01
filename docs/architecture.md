# Architecture notes

Phase 1 contains a single command-line executable with no networking or Raft state.

Later phases will introduce independent components for:

- Node lifecycle and consensus state
- Election and replication RPC handling
- TCP transport and message framing
- Persistent Raft log and metadata
- Deterministic key-value state machine
- Automated multi-process cluster testing

All scope statements above are **planned**, not implemented.
