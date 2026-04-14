#include "raft_commit.h"
#include "raft_config.h"
#include <stdint.h>

raft_status_t raft_commit_advance(raft_node_t *leader,
                                  const raft_replication_progress_t *followers,
                                  size_t follower_count,
                                  raft_index_t *out_commit)
{
    if (leader == NULL || followers == NULL || out_commit == NULL ||
        follower_count != RAFT_CLUSTER_SIZE - 1 ||
        !raft_node_is_valid(leader) || leader->role != RAFT_LEADER ||
        leader->current_term == 0) return RAFT_ERR_INVALID_ARGUMENT;

    const raft_index_t last = raft_log_last_index(&leader->log);
    if (last == UINT64_MAX) return RAFT_ERR_INVALID_ARGUMENT;
    raft_index_t indexes[RAFT_CLUSTER_SIZE];
    indexes[0] = last; /* The leader has its own log. */
    for (size_t i = 0; i < follower_count; ++i) {
        const raft_replication_progress_t *p = &followers[i];
        if (p->peer_id == RAFT_NO_VOTE || p->peer_id == leader->id ||
            p->term != leader->current_term || p->next_index == 0 ||
            p->next_index > last + 1 || p->match_index >= p->next_index ||
            p->match_index > last)
            return RAFT_ERR_INVALID_ARGUMENT;
        for (size_t j = 0; j < i; ++j)
            if (followers[j].peer_id == p->peer_id)
                return RAFT_ERR_INVALID_ARGUMENT;
        indexes[i + 1] = p->match_index;
    }

    /* Sort the three known indexes in descending order. The second-highest
     * index is the greatest replicated by at least two voters. */
    for (size_t i = 0; i < RAFT_CLUSTER_SIZE; ++i) {
        for (size_t j = i + 1; j < RAFT_CLUSTER_SIZE; ++j) {
            if (indexes[j] > indexes[i]) {
                raft_index_t tmp = indexes[i];
                indexes[i] = indexes[j];
                indexes[j] = tmp;
            }
        }
    }
    const raft_index_t majority_index = indexes[RAFT_CLUSTER_SIZE / 2];
    if (majority_index > leader->commit_index) {
        raft_term_t term = 0;
        if (raft_log_term_at(&leader->log, majority_index, &term) != RAFT_OK)
            return RAFT_ERR_INVALID_ARGUMENT;
        /* Older-term entries become committed indirectly only when a
         * current-term entry at or after them reaches a majority. */
        if (term == leader->current_term)
            leader->commit_index = majority_index;
    }
    *out_commit = leader->commit_index;
    return RAFT_OK;
}
