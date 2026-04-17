#ifndef RAFT_WAL_H
#define RAFT_WAL_H

#include "raft_storage.h"
#include <stdint.h>

/* Phase 17: opt-in, append-only, fsync-before-ack incremental Raft journal.
 * The node-ID lock is shared with Phase 16; the two disk formats cannot
 * silently share a node ID/data directory. This format is not compacted. */
#define RAFT_WAL_MAX_FILE_BYTES (64u * 1024u * 1024u)

typedef struct {
    raft_storage_t lock;
    int fd;
    raft_node_t previous; /* owned copy of last durable consensus state */
    uint64_t sequence;
    int loaded;
    char file_name[48];
} raft_wal_t;

/* Open and lock, create a new header if absent; refuse a Phase 16 snapshot.
 * Caller must call load before any append. */
int raft_wal_open(raft_wal_t *wal, const char *directory, raft_node_id_t id,
                  char *error, size_t capacity);
/* Complete damaged records are rejected; an incomplete final record is
 * discarded with ftruncate+fsync before future appends. On failure the node
 * is unchanged. Caller reconstructs timer and applied KV after loading. */
int raft_wal_load(raft_wal_t *wal, raft_node_t *node, int *found,
                  char *error, size_t capacity);
/* Appends only changed metadata and the changed log suffix, then fsyncs.
 * Must be serialized by the node mutex, before consensus acknowledgments. */
int raft_wal_save(raft_wal_t *wal, const raft_node_t *node,
                  char *error, size_t capacity);
void raft_wal_close(raft_wal_t *wal);

#endif
