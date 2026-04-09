#ifndef RAFT_ELECTION_H
#define RAFT_ELECTION_H

#include "raft_vote.h"

/* This milestone assumes exactly three configured voting members. */
#define RAFT_ELECTION_CLUSTER_SIZE 3u
#define RAFT_ELECTION_MAJORITY 2u

typedef struct {
    raft_term_t term;
    unsigned int votes; /* Includes the candidate's own vote. */
    raft_node_id_t counted_peers[RAFT_ELECTION_CLUSTER_SIZE];
    unsigned int counted_count;
} raft_election_round_t;

/* All functions require external synchronization for shared raft_node_t.
 * Term/voted_for are in memory only: crash-safe voting comes later. */
raft_status_t raft_election_begin(raft_node_t *node, uint64_t now_ms,
                                  raft_election_round_t *round);
/* Processes a reply from a configured peer. Duplicate/stale replies cannot
 * elect a leader. Higher-term replies cause immediate follower transition. */
raft_status_t raft_election_record(raft_node_t *node,
                                    raft_election_round_t *round,
                                    raft_node_id_t peer_id,
                                    const raft_vote_response_t *reply,
                                    uint64_t now_ms);

#endif
