#ifndef RAFT_STORAGE_H
#define RAFT_STORAGE_H

#include "raft_node.h"
#include <stddef.h>

/* Phase 16: opt-in, single-file, atomic durable snapshot per node.
 * Do not share one node ID between concurrently running processes.
 * A per-node flock prevents concurrent writers, including restarts.
 * This is educational durable groundwork, not a WAL or snapshot compaction.
 */
#define RAFT_STORAGE_MAX_BYTES (4u * 1024u * 1024u)
#define RAFT_STORAGE_MAX_ENTRIES 4096u
#define RAFT_STORAGE_MAX_COMMAND 4096u

typedef struct {
    int directory_fd;
    int lock_fd;
    raft_node_id_t id;
    char data_name[48];
    char temp_name[48];
} raft_storage_t;

/* Directory is created (mode 0700) if absent; symbolic-link directory is
 * rejected. File mode is 0600. Errors are written to the optional buffer. */
int raft_storage_open(raft_storage_t *store, const char *directory,
                      raft_node_id_t id, char *error, size_t capacity);
void raft_storage_close(raft_storage_t *store);
/* Missing file is a clean bootstrap (found=0). Corrupt/truncated files fail.
 * On failure the caller's node is unchanged. On success the node becomes a
 * follower and last_applied=0 for deterministic state machine replay. */
int raft_storage_load(raft_storage_t *store, raft_node_t *node, int *found,
                      char *error, size_t capacity);
/* Writes temp, fsyncs file, renames atomically and fsyncs the directory.
 * Must be called with the node mutex held in the TCP server. */
int raft_storage_save(raft_storage_t *store, const raft_node_t *node,
                      char *error, size_t capacity);

#endif
