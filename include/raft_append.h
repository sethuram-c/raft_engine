#ifndef RAFT_APPEND_H
#define RAFT_APPEND_H

#include "raft_node.h"
#include <stddef.h>
#include <stdint.h>

/* Existing heartbeat header: term:u64, leader_id:u32, prev_log_index:u64,
 * prev_log_term:u64, leader_commit:u64. All integers big endian. */
#define RAFT_APPEND_REQUEST_BYTES 36u
#define RAFT_APPEND_RESPONSE_BYTES 9u
#define RAFT_APPEND_MAX_ENTRIES 16u

/* Phase 12 entry-bearing extension:
 * header[36] + count:u16 + repeated(term:u64, command_len:u16, bytes[]).
 * A 36-byte header alone remains a backwards-compatible empty heartbeat.
 * A decoded batch BORROWS its command bytes from the input buffer. */
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

typedef struct {
    raft_append_request_t header;
    size_t entry_count;
    raft_log_entry_t entries[RAFT_APPEND_MAX_ENTRIES];
} raft_append_batch_t;

raft_status_t raft_append_encode_request(const raft_append_request_t *request,
                                         uint8_t out[RAFT_APPEND_REQUEST_BYTES]);
raft_status_t raft_append_decode_request(const uint8_t *data, size_t length,
                                         raft_append_request_t *request);
raft_status_t raft_append_encode_response(const raft_append_response_t *response,
                                          uint8_t out[RAFT_APPEND_RESPONSE_BYTES]);
raft_status_t raft_append_decode_response(const uint8_t *data, size_t length,
                                          raft_append_response_t *response);

/* Never writes a partial result on invalid input; only a successfully encoded
 * frame may be transmitted. Fails if the fixed wire payload limit is exceeded. */
raft_status_t raft_append_encode_batch(const raft_append_batch_t *batch,
                                       uint8_t *out, size_t capacity,
                                       size_t *written);
/* On success, entries[i].command points INTO data, NOT allocated memory.
 * Keep the input buffer alive until raft_append_handle_batch() returns. */
raft_status_t raft_append_decode_batch(const uint8_t *data, size_t length,
                                       raft_append_batch_t *out);

/* Caller validates configured leader ID, and holds the node mutex. In-memory
 * only; commit_index is not a majority acknowledgement of this append. */
raft_status_t raft_append_handle_batch(raft_node_t *node,
                                       const raft_append_batch_t *batch,
                                       uint64_t now_ms,
                                       raft_append_response_t *response);
/* Backwards-compatible zero-entry heartbeat handler. */
raft_status_t raft_append_handle(raft_node_t *node,
                                 const raft_append_request_t *request,
                                 uint64_t now_ms,
                                 raft_append_response_t *response);
raft_status_t raft_append_handle_response(raft_node_t *node,
                                          const raft_append_response_t *response,
                                          uint64_t now_ms);
#endif
