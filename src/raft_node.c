#include "raft_node.h"

#include <string.h>

raft_status_t raft_node_init(raft_node_t *node, raft_node_id_t id)
{
    if (node == NULL || id == RAFT_NO_VOTE) {
        return RAFT_ERR_INVALID_ARGUMENT;
    }
    memset(node, 0, sizeof(*node));
    node->id = id;
    node->role = RAFT_FOLLOWER;
    node->voted_for = RAFT_NO_VOTE;
    raft_log_init(&node->log);
    return RAFT_OK;
}

void raft_node_destroy(raft_node_t *node)
{
    if (node == NULL) {
        return;
    }
    raft_log_destroy(&node->log);
    memset(node, 0, sizeof(*node));
}

const char *raft_role_name(raft_role_t role)
{
    switch (role) {
        case RAFT_FOLLOWER:  return "follower";
        case RAFT_CANDIDATE: return "candidate";
        case RAFT_LEADER:    return "leader";
        default:             return "unknown";
    }
}

int raft_node_is_valid(const raft_node_t *node)
{
    if (node == NULL || node->id == RAFT_NO_VOTE) {
        return 0;
    }
    if (node->role != RAFT_FOLLOWER && node->role != RAFT_CANDIDATE &&
        node->role != RAFT_LEADER) {
        return 0;
    }
    if (node->last_applied > node->commit_index ||
        node->commit_index > raft_log_last_index(&node->log)) {
        return 0;
    }
    if (node->log.count > node->log.capacity ||
        (node->log.count > 0 && node->log.entries == NULL)) {
        return 0;
    }

    raft_term_t previous = 0;
    for (size_t i = 0; i < node->log.count; ++i) {
        const raft_log_entry_t *entry = &node->log.entries[i];
        if (entry->term == 0 || entry->term < previous ||
            entry->term > node->current_term ||
            (entry->command_len != 0 && entry->command == NULL)) {
            return 0;
        }
        previous = entry->term;
    }
    return 1;
}
