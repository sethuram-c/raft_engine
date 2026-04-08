#include "raft_vote.h"

#include <string.h>

static void write_u32(uint8_t *p, uint32_t x)
{
    p[0] = (uint8_t)(x >> 24);
    p[1] = (uint8_t)(x >> 16);
    p[2] = (uint8_t)(x >> 8);
    p[3] = (uint8_t)x;
}

static uint32_t read_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void write_u64(uint8_t *p, uint64_t x)
{
    for (size_t i = 0; i < 8; ++i)
        p[i] = (uint8_t)(x >> (56u - 8u * (unsigned)i));
}

static uint64_t read_u64(const uint8_t *p)
{
    uint64_t x = 0;
    for (size_t i = 0; i < 8; ++i) x = (x << 8) | p[i];
    return x;
}

static int valid_request(const raft_vote_request_t *req)
{
    return req != NULL && req->term != 0 && req->candidate_id != RAFT_NO_VOTE &&
           req->last_log_term <= req->term &&
           ((req->last_log_index == 0 && req->last_log_term == 0) ||
            (req->last_log_index > 0 && req->last_log_term > 0));
}

raft_status_t raft_vote_encode_request(const raft_vote_request_t *req,
                                      uint8_t output[RAFT_VOTE_REQUEST_BYTES])
{
    if (output == NULL || !valid_request(req)) return RAFT_ERR_INVALID_ARGUMENT;
    write_u64(output, req->term);
    write_u32(output + 8, req->candidate_id);
    write_u64(output + 12, req->last_log_index);
    write_u64(output + 20, req->last_log_term);
    return RAFT_OK;
}

raft_status_t raft_vote_decode_request(const uint8_t *bytes, size_t length,
                                      raft_vote_request_t *req)
{
    if (bytes == NULL || req == NULL || length != RAFT_VOTE_REQUEST_BYTES)
        return RAFT_ERR_INVALID_ARGUMENT;
    raft_vote_request_t tmp = {
        .term = read_u64(bytes), .candidate_id = read_u32(bytes + 8),
        .last_log_index = read_u64(bytes + 12),
        .last_log_term = read_u64(bytes + 20)
    };
    if (!valid_request(&tmp)) return RAFT_ERR_INVALID_ARGUMENT;
    *req = tmp;
    return RAFT_OK;
}

raft_status_t raft_vote_encode_response(const raft_vote_response_t *rsp,
                                       uint8_t output[RAFT_VOTE_RESPONSE_BYTES])
{
    if (rsp == NULL || output == NULL || (rsp->vote_granted != 0 && rsp->vote_granted != 1))
        return RAFT_ERR_INVALID_ARGUMENT;
    write_u64(output, rsp->term);
    output[8] = (uint8_t)rsp->vote_granted;
    return RAFT_OK;
}

raft_status_t raft_vote_decode_response(const uint8_t *bytes, size_t length,
                                       raft_vote_response_t *rsp)
{
    if (bytes == NULL || rsp == NULL || length != RAFT_VOTE_RESPONSE_BYTES ||
        bytes[8] > 1) return RAFT_ERR_INVALID_ARGUMENT;
    raft_vote_response_t tmp = {.term = read_u64(bytes), .vote_granted = bytes[8]};
    *rsp = tmp;
    return RAFT_OK;
}

raft_status_t raft_vote_handle(raft_node_t *node,
                              const raft_vote_request_t *req,
                              uint64_t now_ms, raft_vote_response_t *rsp)
{
    if (node == NULL || rsp == NULL || !valid_request(req) ||
        !raft_node_is_valid(node) || !node->election_timer.armed)
        return RAFT_ERR_INVALID_ARGUMENT;

    raft_term_t next_term = req->term > node->current_term ? req->term : node->current_term;
    raft_node_id_t next_vote = req->term > node->current_term ? RAFT_NO_VOTE : node->voted_for;
    int up_to_date = 0;
    raft_index_t local_index = raft_log_last_index(&node->log);
    raft_term_t local_term = 0;
    if (raft_log_term_at(&node->log, local_index, &local_term) != RAFT_OK)
        return RAFT_ERR_INVALID_ARGUMENT;
    up_to_date = req->last_log_term > local_term ||
        (req->last_log_term == local_term && req->last_log_index >= local_index);
    int granted = req->term >= node->current_term && up_to_date &&
                  (next_vote == RAFT_NO_VOTE || next_vote == req->candidate_id);

    /* Check timer reset before modifying node, so errors cannot grant an
     * unrecorded vote. The node's mutex is held by the TCP handler. */
    raft_election_timer_t timer = node->election_timer;
    if (granted) {
        raft_status_t status = raft_election_timer_reset(&timer, now_ms);
        if (status != RAFT_OK) return status;
    }
    if (req->term > node->current_term) {
        node->current_term = next_term;
        node->voted_for = RAFT_NO_VOTE;
        node->role = RAFT_FOLLOWER;
    }
    if (granted) {
        node->role = RAFT_FOLLOWER;
        node->voted_for = req->candidate_id;
        node->election_timer = timer;
    }
    rsp->term = node->current_term;
    rsp->vote_granted = granted;
    return RAFT_OK;
}
