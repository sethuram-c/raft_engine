#include "raft_append.h"

static void put_u32(uint8_t *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (24u - 8u*i));
}
static void put_u64(uint8_t *p, uint64_t n)
{
    for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(n >> (56u - 8u*i));
}
static uint32_t get_u32(const uint8_t *p)
{
    uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i) n = (n << 8) | p[i];
    return n;
}
static uint64_t get_u64(const uint8_t *p)
{
    uint64_t n = 0;
    for (unsigned i = 0; i < 8; ++i) n = (n << 8) | p[i];
    return n;
}
static int valid_request(const raft_append_request_t *r)
{
    return r != NULL && r->term != 0 && r->leader_id != RAFT_NO_VOTE &&
           r->prev_log_term <= r->term &&
           ((r->prev_log_index == 0 && r->prev_log_term == 0) ||
            (r->prev_log_index > 0 && r->prev_log_term != 0));
}

raft_status_t raft_append_encode_request(const raft_append_request_t *r,
                                        uint8_t out[RAFT_APPEND_REQUEST_BYTES])
{
    if (!valid_request(r) || out == NULL) return RAFT_ERR_INVALID_ARGUMENT;
    put_u64(out, r->term);
    put_u32(out+8, r->leader_id);
    put_u64(out+12, r->prev_log_index);
    put_u64(out+20, r->prev_log_term);
    put_u64(out+28, r->leader_commit);
    return RAFT_OK;
}

raft_status_t raft_append_decode_request(const uint8_t *data, size_t len,
                                        raft_append_request_t *r)
{
    if (data == NULL || r == NULL || len != RAFT_APPEND_REQUEST_BYTES)
        return RAFT_ERR_INVALID_ARGUMENT;
    raft_append_request_t tmp = {.term=get_u64(data), .leader_id=get_u32(data+8),
        .prev_log_index=get_u64(data+12), .prev_log_term=get_u64(data+20),
        .leader_commit=get_u64(data+28)};
    if (!valid_request(&tmp)) return RAFT_ERR_INVALID_ARGUMENT;
    *r = tmp;
    return RAFT_OK;
}

raft_status_t raft_append_encode_response(const raft_append_response_t *r,
                                         uint8_t out[RAFT_APPEND_RESPONSE_BYTES])
{
    if (r == NULL || out == NULL || (r->success != 0 && r->success != 1))
        return RAFT_ERR_INVALID_ARGUMENT;
    put_u64(out, r->term);
    out[8] = (uint8_t)r->success;
    return RAFT_OK;
}

raft_status_t raft_append_decode_response(const uint8_t *data, size_t len,
                                         raft_append_response_t *r)
{
    if (data == NULL || r == NULL || len != RAFT_APPEND_RESPONSE_BYTES || data[8] > 1)
        return RAFT_ERR_INVALID_ARGUMENT;
    raft_append_response_t tmp = {.term=get_u64(data), .success=data[8]};
    *r = tmp;
    return RAFT_OK;
}

raft_status_t raft_append_handle(raft_node_t *node,
                                 const raft_append_request_t *r,
                                 uint64_t now_ms, raft_append_response_t *reply)
{
    if (node == NULL || reply == NULL || !valid_request(r) ||
        !raft_node_is_valid(node) || !node->election_timer.armed ||
        r->leader_id == node->id)
        return RAFT_ERR_INVALID_ARGUMENT;

    if (r->term < node->current_term) {
        *reply = (raft_append_response_t){node->current_term, 0};
        return RAFT_OK;
    }

    /* Do not mutate the term/role until we know timer reset can succeed. */
    raft_election_timer_t timer = node->election_timer;
    raft_status_t result = raft_election_timer_reset(&timer, now_ms);
    if (result != RAFT_OK) return result;

    if (r->term > node->current_term) {
        node->current_term = r->term;
        node->voted_for = RAFT_NO_VOTE;
    }
    /* Recognizing a leader in our term demotes any former candidate/leader. */
    node->role = RAFT_FOLLOWER;
    node->election_timer = timer;

    raft_term_t local_prev_term = 0;
    int consistent = raft_log_term_at(&node->log, r->prev_log_index,
                                     &local_prev_term) == RAFT_OK &&
                     local_prev_term == r->prev_log_term;
    if (consistent && r->leader_commit > node->commit_index) {
        /* With zero entries, lastNewEntryIndex = prev_log_index. */
        raft_index_t target = r->leader_commit < r->prev_log_index ?
                              r->leader_commit : r->prev_log_index;
        if (target > node->commit_index) node->commit_index = target;
    }
    *reply = (raft_append_response_t){node->current_term, consistent};
    return RAFT_OK;
}

raft_status_t raft_append_handle_response(raft_node_t *node,
                                         const raft_append_response_t *r,
                                         uint64_t now_ms)
{
    if (node == NULL || r == NULL || (r->success != 0 && r->success != 1) ||
        !raft_node_is_valid(node) || !node->election_timer.armed)
        return RAFT_ERR_INVALID_ARGUMENT;
    if (r->term <= node->current_term) return RAFT_OK;
    raft_election_timer_t timer = node->election_timer;
    raft_status_t result = raft_election_timer_reset(&timer, now_ms);
    if (result != RAFT_OK) return result;
    node->role = RAFT_FOLLOWER;
    node->voted_for = RAFT_NO_VOTE;
    node->current_term = r->term;
    node->election_timer = timer;
    return RAFT_OK;
}
