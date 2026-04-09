#include "raft_election.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

static raft_node_t make_node(void)
{
    raft_node_t node;
    assert(raft_node_init(&node, 1) == RAFT_OK);
    assert(raft_node_start_follower(&node, 100, 101) == RAFT_OK);
    return node;
}

int main(void)
{
    raft_node_t n = make_node();
    raft_election_round_t r;
    assert(raft_election_begin(&n, 800, &r) == RAFT_OK);
    assert(n.role == RAFT_CANDIDATE && n.current_term == 1 && n.voted_for == 1);
    assert(r.term == 1 && r.votes == 1 && r.counted_count == 1);
    assert(n.election_timer.deadline_ms > 800);
    raft_vote_response_t stale = {0, 1};
    assert(raft_election_record(&n, &r, 2, &stale, 810) == RAFT_OK);
    assert(n.role == RAFT_CANDIDATE && r.votes == 1);
    raft_vote_response_t denied = {1, 0};
    assert(raft_election_record(&n, &r, 2, &denied, 810) == RAFT_OK);
    assert(r.votes == 1);
    raft_vote_response_t granted = {1, 1};
    assert(raft_election_record(&n, &r, 2, &granted, 810) == RAFT_OK);
    assert(r.votes == 2 && n.role == RAFT_LEADER);
    assert(raft_election_record(&n, &r, 2, &granted, 810) == RAFT_OK);
    assert(r.votes == 2); /* duplicate cannot be counted twice */
    assert(raft_election_begin(&n, 820, &r) == RAFT_ERR_INVALID_ARGUMENT);
    raft_vote_response_t higher = {3, 0};
    assert(raft_election_record(&n, &r, 3, &higher, 825) == RAFT_OK);
    assert(n.role == RAFT_FOLLOWER && n.current_term == 3 && n.voted_for == 0);
    assert(raft_election_record(&n, &r, 2, &granted, 830) == RAFT_OK);
    assert(n.role == RAFT_FOLLOWER);
    assert(raft_election_begin(&n, 1200, &r) == RAFT_OK);
    assert(n.role == RAFT_CANDIDATE && n.current_term == 4 && r.term == 4);
    assert(raft_election_begin(&n, 2000, &r) == RAFT_OK);
    assert(n.current_term == 5 && r.votes == 1); /* retry */
    raft_vote_response_t invalid = {5, 2};
    assert(raft_election_record(&n, &r, 2, &invalid, 2001) == RAFT_ERR_INVALID_ARGUMENT);
    assert(raft_election_record(&n, &r, 1, &granted, 2001) == RAFT_ERR_INVALID_ARGUMENT);
    n.current_term = UINT64_MAX;
    assert(raft_election_begin(&n, 2200, &r) == RAFT_ERR_INVALID_ARGUMENT);
    raft_node_destroy(&n);
    puts("PASS: election start, retry, quorum, duplicate, stale, step-down, limits");
    return 0;
}
