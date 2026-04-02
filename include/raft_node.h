#ifndef RAFT_NODE_H
#define RAFT_NODE_H

#include "raft_log.h"

typedef struct {
    raft_node_id_t id;
    raft_role_t role;
    raft_term_t current_term;
    raft_node_id_t voted_for;
    raft_index_t commit_index;
    raft_index_t last_applied;
    raft_log_t log;
} raft_node_t;

/* This phase is single-threaded and in-memory; it does not run Raft. */
raft_status_t raft_node_init(raft_node_t *node, raft_node_id_t id);
void raft_node_destroy(raft_node_t *node);

/* Check local structural invariants, not network/consensus correctness. */
int raft_node_is_valid(const raft_node_t *node);
const char *raft_role_name(raft_role_t role);

#endif /* RAFT_NODE_H */
