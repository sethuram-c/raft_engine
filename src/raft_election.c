#include "raft_election.h"
#include <limits.h>
#include <string.h>

raft_status_t raft_election_begin(raft_node_t *node, uint64_t now_ms,
                                  raft_election_round_t *round)
{
    if (node == NULL || round == NULL || !raft_node_is_valid(node) ||
        !node->election_timer.armed || node->role == RAFT_LEADER ||
        node->current_term == UINT64_MAX)
        return RAFT_ERR_INVALID_ARGUMENT;
    raft_election_timer_t next_timer = node->election_timer;
    raft_status_t status = raft_election_timer_reset(&next_timer, now_ms);
    if (status != RAFT_OK) return status;
    node->current_term++;
    node->role = RAFT_CANDIDATE;
    node->voted_for = node->id;
    node->election_timer = next_timer;
    memset(round, 0, sizeof(*round));
    round->term = node->current_term;
    round->votes = 1;
    round->counted_peers[0] = node->id;
    round->counted_count = 1;
    return RAFT_OK;
}

raft_status_t raft_election_record(raft_node_t *node,
                                    raft_election_round_t *round,
                                    raft_node_id_t peer_id,
                                    const raft_vote_response_t *reply,
                                    uint64_t now_ms)
{
    if (node == NULL || round == NULL || reply == NULL ||
        peer_id == RAFT_NO_VOTE || peer_id == node->id ||
        (reply->vote_granted != 0 && reply->vote_granted != 1) ||
        !raft_node_is_valid(node)) return RAFT_ERR_INVALID_ARGUMENT;

    if (reply->term > node->current_term) {
        raft_election_timer_t timer = node->election_timer;
        raft_status_t status = raft_election_timer_reset(&timer, now_ms);
        if (status != RAFT_OK) return status;
        node->current_term = reply->term;
        node->role = RAFT_FOLLOWER;
        node->voted_for = RAFT_NO_VOTE;
        node->election_timer = timer;
        return RAFT_OK;
    }
    if (node->role != RAFT_CANDIDATE || node->current_term != round->term ||
        reply->term != round->term || !reply->vote_granted) return RAFT_OK;
    for (unsigned int i = 0; i < round->counted_count; ++i)
        if (round->counted_peers[i] == peer_id) return RAFT_OK;
    if (round->counted_count >= RAFT_ELECTION_CLUSTER_SIZE)
        return RAFT_ERR_OUT_OF_RANGE;
    round->counted_peers[round->counted_count++] = peer_id;
    round->votes++;
    if (round->votes >= RAFT_ELECTION_MAJORITY) node->role = RAFT_LEADER;
    return RAFT_OK;
}
