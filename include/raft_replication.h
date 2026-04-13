#ifndef RAFT_REPLICATION_H
#define RAFT_REPLICATION_H

#include "raft_append.h"
#include "raft_wire.h"

/* Leader-owned progress for one voting peer; callers hold node mutex. */
typedef struct {
    raft_node_id_t peer_id;
    raft_term_t term;
    raft_index_t next_index;  /* First entry to send; starts at leader_last+1. */
    raft_index_t match_index; /* Conservatively confirmed replicated prefix. */
} raft_replication_progress_t;

/* Starts a new term's follower progress. */
raft_status_t raft_replication_progress_init(raft_replication_progress_t *progress,
                                            raft_node_id_t peer_id, raft_term_t term,
                                            raft_index_t leader_last);
/* Borrowed command pointers; build/encode while holding the leader mutex.
 * Builds a bounded batch (<=16 entries, <=4096 wire bytes), or a heartbeat.
 * sent_through is the last included index, equal to prev for empty batches. */
raft_status_t raft_replication_prepare(const raft_node_t *leader,
                                       const raft_replication_progress_t *progress,
                                       raft_append_batch_t *batch,
                                       raft_index_t *sent_through);
/* Only SAME-term replies adjust progress. Higher-term responses must first
 * be passed to raft_append_handle_response() to step down the node. */
raft_status_t raft_replication_observe(raft_replication_progress_t *progress,
                                       raft_index_t sent_through,
                                       const raft_append_response_t *reply);
#endif
