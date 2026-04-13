#include "raft_replication.h"
#include <stdint.h>
#include <string.h>

raft_status_t raft_replication_progress_init(raft_replication_progress_t *progress,
                                            raft_node_id_t peer_id, raft_term_t term,
                                            raft_index_t leader_last)
{
    if (progress == NULL || peer_id == RAFT_NO_VOTE || term == 0 ||
        leader_last == UINT64_MAX) return RAFT_ERR_INVALID_ARGUMENT;
    *progress = (raft_replication_progress_t){peer_id, term, leader_last + 1, 0};
    return RAFT_OK;
}

raft_status_t raft_replication_prepare(const raft_node_t *leader,
                                       const raft_replication_progress_t *progress,
                                       raft_append_batch_t *batch,
                                       raft_index_t *sent_through)
{
    if (leader == NULL || progress == NULL || batch == NULL || sent_through == NULL ||
        !raft_node_is_valid(leader) || leader->role != RAFT_LEADER ||
        leader->current_term != progress->term || progress->term == 0 ||
        progress->peer_id == RAFT_NO_VOTE || progress->peer_id == leader->id ||
        progress->next_index == 0) return RAFT_ERR_INVALID_ARGUMENT;
    raft_index_t last = raft_log_last_index(&leader->log);
    if (last == UINT64_MAX || progress->next_index > last + 1)
        return RAFT_ERR_INVALID_ARGUMENT;
    raft_index_t prev = progress->next_index - 1;
    raft_term_t prev_term = 0;
    if (raft_log_term_at(&leader->log, prev, &prev_term) != RAFT_OK)
        return RAFT_ERR_INVALID_ARGUMENT;
    raft_append_batch_t out = {0};
    out.header = (raft_append_request_t){leader->current_term, leader->id, prev,
                                         prev_term, leader->commit_index};
    size_t size = RAFT_APPEND_REQUEST_BYTES + 2;
    for (raft_index_t index = progress->next_index;
         index <= last && out.entry_count < RAFT_APPEND_MAX_ENTRIES; ++index) {
        const raft_log_entry_t *entry = raft_log_at(&leader->log, index);
        if (entry == NULL || entry->command_len > UINT16_MAX ||
            (entry->command_len && entry->command == NULL))
            return RAFT_ERR_INVALID_ARGUMENT;
        if (entry->command_len > RAFT_WIRE_MAX_PAYLOAD - size - 10) {
            if (out.entry_count == 0) return RAFT_ERR_INVALID_ARGUMENT;
            break;
        }
        out.entries[out.entry_count++] = *entry; /* borrowed bytes */
        size += 10 + entry->command_len;
    }
    *sent_through = prev + (raft_index_t)out.entry_count;
    *batch = out;
    return RAFT_OK;
}

raft_status_t raft_replication_observe(raft_replication_progress_t *progress,
                                       raft_index_t sent_through,
                                       const raft_append_response_t *reply)
{
    if (progress == NULL || reply == NULL || progress->term == 0 ||
        progress->next_index == 0 || (reply->success != 0 && reply->success != 1) ||
        reply->term > progress->term || sent_through < progress->next_index - 1)
        return RAFT_ERR_INVALID_ARGUMENT;
    if (reply->term < progress->term) return RAFT_OK; /* stale */
    if (reply->success) {
        if (sent_through > progress->match_index) progress->match_index = sent_through;
        if (sent_through != UINT64_MAX && sent_through + 1 > progress->next_index)
            progress->next_index = sent_through + 1;
    } else {
        if (progress->next_index > 1) --progress->next_index;
        /* Phase 13 uses memory-only followers. Restart can discard acknowledged
         * entries; conservatively lower matchIndex while backing off. */
        if (progress->match_index >= progress->next_index)
            progress->match_index = progress->next_index - 1;
    }
    return RAFT_OK;
}
