#ifndef RAFT_VOTE_H
#define RAFT_VOTE_H

#include "raft_node.h"
#include <stddef.h>
#include <stdint.h>

/* Fixed-size big-endian payloads carried inside Phase 5 wire frames. */
#define RAFT_VOTE_REQUEST_BYTES 28u
#define RAFT_VOTE_RESPONSE_BYTES 9u

typedef struct {
    raft_term_t term;
    raft_node_id_t candidate_id;
    raft_index_t last_log_index;
    raft_term_t last_log_term;
} raft_vote_request_t;

typedef struct {
    raft_term_t term;
    int vote_granted; /* exactly 0 or 1 */
} raft_vote_response_t;

/* Encode/decode all fields explicitly; never transmit raw C struct layouts.
 * Returns RAFT_OK on success; malformed payloads are rejected without
 * modifying the output struct. */
raft_status_t raft_vote_encode_request(const raft_vote_request_t *request,
                                      uint8_t output[RAFT_VOTE_REQUEST_BYTES]);
raft_status_t raft_vote_decode_request(const uint8_t *bytes, size_t length,
                                      raft_vote_request_t *request);
raft_status_t raft_vote_encode_response(const raft_vote_response_t *response,
                                       uint8_t output[RAFT_VOTE_RESPONSE_BYTES]);
raft_status_t raft_vote_decode_response(const uint8_t *bytes, size_t length,
                                       raft_vote_response_t *response);

/* Follower-side RequestVote logic (Raft 5.2 and 5.4.1). Caller serializes
 * concurrent access to node, and must have armed its follower timer.
 * This phase does not persist term/voted_for, so vote safety does not survive
 * restart. The next phases will add persistent state and real elections. */
raft_status_t raft_vote_handle(raft_node_t *node,
                              const raft_vote_request_t *request,
                              uint64_t now_ms, raft_vote_response_t *response);

#endif
