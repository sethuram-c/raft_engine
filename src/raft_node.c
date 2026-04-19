#include "raft_node.h"
#include <string.h>

raft_status_t raft_node_init(raft_node_t *node, raft_node_id_t id)
{
    if (node == NULL || id == RAFT_NO_VOTE) return RAFT_ERR_INVALID_ARGUMENT;
    memset(node, 0, sizeof(*node));
    node->id = id;
    node->role = RAFT_FOLLOWER;
    node->voted_for = RAFT_NO_VOTE;
    raft_log_init(&node->log);
    return RAFT_OK;
}

void raft_node_destroy(raft_node_t *node)
{
    if (node == NULL) return;
    raft_log_destroy(&node->log);
    memset(node, 0, sizeof(*node));
}

int raft_node_is_valid(const raft_node_t *node)
{
    if (node == NULL || node->id == RAFT_NO_VOTE ||
        node->role < RAFT_FOLLOWER || node->role > RAFT_LEADER ||
        node->log.count > node->log.capacity ||
        node->log.base_index > UINT64_MAX - (raft_index_t)node->log.count ||
        (node->log.base_index == 0) != (node->log.base_term == 0) ||
        node->log.base_term > node->current_term ||
        node->last_applied < node->log.base_index ||
        node->commit_index < node->log.base_index ||
        (node->log.count != 0 && node->log.entries == NULL) ||
        node->last_applied > node->commit_index ||
        node->commit_index > raft_log_last_index(&node->log)) return 0;
    const raft_election_timer_t *timer = &node->election_timer;
    if (timer->armed && (timer->min_ms == 0 ||
        timer->max_ms < timer->min_ms ||
        timer->timeout_ms < timer->min_ms ||
        timer->timeout_ms > timer->max_ms ||
        timer->rng_state == 0 ||
        timer->deadline_ms < timer->timeout_ms)) return 0;
    raft_term_t previous = node->log.base_term;
    for (size_t i = 0; i < node->log.count; ++i) {
        const raft_log_entry_t *entry = &node->log.entries[i];
        if (entry->term == 0 || entry->term < previous ||
            entry->term > node->current_term ||
            (entry->command_len != 0 && entry->command == NULL)) return 0;
        previous = entry->term;
    }
    return 1;
}

const char *raft_role_name(raft_role_t role)
{
    switch (role) {
    case RAFT_FOLLOWER: return "follower";
    case RAFT_CANDIDATE: return "candidate";
    case RAFT_LEADER: return "leader";
    default: return "unknown";
    }
}

raft_status_t raft_node_start_follower(raft_node_t *node, uint64_t now_ms,
                                      uint32_t seed)
{
    if (node == NULL || node->id == RAFT_NO_VOTE)
        return RAFT_ERR_INVALID_ARGUMENT;
    raft_election_timer_t timer;
    raft_status_t result = raft_election_timer_init(&timer,
        RAFT_ELECTION_TIMEOUT_MIN_MS, RAFT_ELECTION_TIMEOUT_MAX_MS, seed);
    if (result != RAFT_OK) return result;
    result = raft_election_timer_reset(&timer, now_ms);
    if (result != RAFT_OK) return result;
    node->role = RAFT_FOLLOWER;
    node->election_timer = timer;
    /* Do not change term or voted_for: those are persistent Raft state. */
    return RAFT_OK;
}

raft_status_t raft_node_reset_follower_timeout(raft_node_t *node,
                                               uint64_t now_ms)
{
    if (node == NULL || node->role != RAFT_FOLLOWER ||
        !node->election_timer.armed) return RAFT_ERR_INVALID_ARGUMENT;
    return raft_election_timer_reset(&node->election_timer, now_ms);
}

int raft_node_follower_timeout_pending(const raft_node_t *node,
                                       uint64_t now_ms)
{
    return node != NULL && node->role == RAFT_FOLLOWER &&
        raft_election_timer_expired(&node->election_timer, now_ms);
}
