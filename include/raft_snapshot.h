#ifndef RAFT_SNAPSHOT_H
#define RAFT_SNAPSHOT_H

#include "raft_state_machine.h"
#include <stddef.h>
#include <stdint.h>

/* Phase 19: offline / in-memory snapshot groundwork. No InstallSnapshot RPC,
 * WAL checkpoint integration, or active-server log truncation yet. A snapshot
 * records the EXACT applied state and log boundary; it does not record a
 * leader term, votes, or uncommitted suffix. */
#define RAFT_SNAPSHOT_MAX_BYTES (256u * 1024u)

typedef struct {
    raft_index_t last_index;
    raft_term_t last_term;
    raft_kv_store_t state;
} raft_snapshot_t;

void raft_snapshot_init(raft_snapshot_t *snapshot);
void raft_snapshot_destroy(raft_snapshot_t *snapshot);
/* Copies state only through lastApplied; does NOT compact the log. */
raft_status_t raft_snapshot_capture(const raft_node_t *node,
                                     const raft_kv_store_t *store,
                                     raft_snapshot_t *out);
/* Uses a versioned, bounded, big-endian, CRC32-protected payload. */
raft_status_t raft_snapshot_encode(const raft_snapshot_t *snapshot,
                                    uint8_t **out_bytes, size_t *out_length);
raft_status_t raft_snapshot_decode(const uint8_t *bytes, size_t length,
                                    raft_snapshot_t *out);
/* Atomic replacement: temporary file, fsync(file), rename, fsync(directory).
 * Requires a trusted directory and an ordinary single-component filename;
 * callers must serialize concurrent writers externally. */
int raft_snapshot_write_file(const char *directory, const char *name,
                             const raft_snapshot_t *snapshot);
int raft_snapshot_read_file(const char *directory, const char *name,
                            raft_snapshot_t *out);
/* Only on a fresh node (empty log, term >= snapshot.last_term). Snapshots
 * have no election metadata, so this cannot replace persisted Raft state. */
raft_status_t raft_snapshot_restore_fresh(const raft_snapshot_t *snapshot,
                                          raft_node_t *node,
                                          raft_kv_store_t *store);
/* Compact only after a separately durable snapshot, and only <= lastApplied.
 * This API is for isolated teaching demos, never called by TCP server. */
raft_status_t raft_snapshot_compact_applied(raft_node_t *node,
                                            const raft_snapshot_t *snapshot);
#endif
