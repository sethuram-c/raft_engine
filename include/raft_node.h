#ifndef RAFT_NODE_H
#define RAFT_NODE_H

#include "raft_log.h"
#include "raft_timer.h"

typedef struct {
    raft_node_id_t id;
    raft_role_t role;
    raft_term_t current_term;
    raft_node_id_t voted_for;
    raft_index_t commit_index;
    raft_index_t last_applied;
    raft_log_t log;
    raft_election_timer_t election_timer;
} raft_node_t;

raft_status_t raft_node_init(raft_node_t *node, raft_node_id_t id);
void raft_node_destroy(raft_node_t *node);
int raft_node_is_valid(const raft_node_t *node);
const char *raft_role_name(raft_role_t role);

/* Follower-timeout bookkeeping only: no vote RPC or role transition yet.
 * All time values are monotonic milliseconds supplied by the caller.
 * These routines are not internally synchronized. */
raft_status_t raft_node_start_follower(raft_node_t *node, uint64_t now_ms,
                                      uint32_t seed);
/* Call only after externally validating leader contact / vote grant. */
raft_status_t raft_node_reset_follower_timeout(raft_node_t *node,
                                               uint64_t now_ms);
int raft_node_follower_timeout_pending(const raft_node_t *node,
                                       uint64_t now_ms);

#endif
