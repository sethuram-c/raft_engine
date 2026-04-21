#ifndef RAFT_SNAPWAL_H
#define RAFT_SNAPWAL_H

#include "raft_snapshot.h"
#include "raft_storage.h"
#include <stdint.h>

/* Phase 21: opt-in, snapshot-aware, single-generation journal. Not a
 * migration of the Phase 17 WAL. The snapshot, log boundary, retained
 * suffix, current term/vote, and commit index share one atomic checkpoint.
 * Between checkpoints, CRC-protected incremental suffix records are fsynced.
 * A single per-node lock protects both the journal and its rename. */
typedef struct {
    raft_storage_t lock;
    int fd;
    raft_node_t previous;
    uint8_t *snapshot;
    size_t snapshot_len;
    uint64_t sequence;
    int loaded;
    char file_name[48];
    char temp_name[64];
} raft_snapwal_t;

int raft_snapwal_open(raft_snapwal_t *wal, const char *directory,
                      raft_node_id_t id, char *error, size_t capacity);
/* Restores consensus state; returns a caller-owned encoded snapshot, if any.
 * Caller must decode it and replay the committed retained suffix before
 * serving requests. On failure 'node' remains unchanged. */
int raft_snapwal_load(raft_snapwal_t *wal, raft_node_t *node, int *found,
                      uint8_t **snapshot, size_t *snapshot_length,
                      char *error, size_t capacity);
int raft_snapwal_save(raft_snapwal_t *wal, const raft_node_t *node,
                      char *error, size_t capacity);
/* Atomically replaces the journal with one self-contained snapshot+suffix
 * record. Pass the current encoded snapshot, already verified to match the
 * log boundary. On ambiguous fsync failures, the caller MUST fail-stop. */
int raft_snapwal_checkpoint(raft_snapwal_t *wal, const raft_node_t *node,
                            const uint8_t *snapshot, size_t length,
                            char *error, size_t capacity);
void raft_snapwal_close(raft_snapwal_t *wal);

#endif
