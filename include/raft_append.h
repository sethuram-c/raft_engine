#ifndef RAFT_APPEND_H
#define RAFT_APPEND_H

#include "raft_node.h"
#include <stddef.h>
#include <stdint.h>

/* Phase 10: zero-entry AppendEntries (heartbeat), not log replication.
 * Fields (big endian): term:u64, leader_id:u32, prev_log_index:u64,
 * prev_log_term:u64, leader_commit:u64. Fixed size = 36 bytes.
 * Response: term:u64, success:u8 (0 or 1), fixed size = 9 bytes.
 */
#define RAFT_APPEND_REQUEST_BYTES 36u
#define RAFT_APPEND_RESPONSE_BYTES 9u

typedef struct {
    raft_term_t term;
    raft_node_id_t leader_id;
    raft_index_t prev_log_index;
    raft_term_t prev_log_term;
    raft_index_t leader_commit;
} raft_append_request_t;

typedef struct {
    raft_term_t term;
    int success;
} raft_append_response_t;

raft_status_t raft_append_encode_request(const raft_append_request_t *request,
                                        uint8_t out[RAFT_APPEND_REQUEST_BYTES]);
raft_status_t raft_append_decode_request(const uint8_t *data, size_t length,
                                        raft_append_request_t *request);
raft_status_t raft_append_encode_response(const raft_append_response_t *response,
                                         uint8_t out[RAFT_APPEND_RESPONSE_BYTES]);
raft_status_t raft_append_decode_response(const uint8_t *data, size_t length,
                                         raft_append_response_t *response);

/* Caller MUST validate that leader_id is a configured peer and MUST protect
 * node with its mutex. Valid leader contact in the current/higher term resets
 * the election timeout EVEN IF the previous-log consistency check fails.
 * This phase carries zero entries; no append or conflict repair is performed.
 */
raft_status_t raft_append_handle(raft_node_t *node,
                                 const raft_append_request_t *request,
                                 uint64_t now_ms,
                                 raft_append_response_t *response);

/* Higher-term AppendEntries response causes the leader to step down. */
raft_status_t raft_append_handle_response(raft_node_t *node,
                                         const raft_append_response_t *response,
                                         uint64_t now_ms);
#endif
