#ifndef RAFT_CLIENT_WRITE_H
#define RAFT_CLIENT_WRITE_H

#include "raft_node.h"
#include <stddef.h>
#include <stdint.h>

/* Phase 22: one operation per request, no idempotency keys yet.
 * Request: timeout_ms:u32 BE, command:[1..1024] raw bytes.
 * Response: term:u64 BE, index:u64 BE, status:u8.
 * A TIMEOUT/UNKNOWN result is INDETERMINATE: the entry may commit later.
 */
#define RAFT_CLIENT_WRITE_MAX_COMMAND 1024u
#define RAFT_CLIENT_WRITE_MIN_TIMEOUT_MS 100u
#define RAFT_CLIENT_WRITE_MAX_TIMEOUT_MS 3000u
#define RAFT_CLIENT_WRITE_RESPONSE_BYTES 17u

typedef enum {
    RAFT_CLIENT_WRITE_COMMITTED = 0,
    RAFT_CLIENT_WRITE_NOT_LEADER = 1,
    RAFT_CLIENT_WRITE_TIMEOUT = 2,
    RAFT_CLIENT_WRITE_REJECTED = 3,
    RAFT_CLIENT_WRITE_UNKNOWN = 4,
    RAFT_CLIENT_WRITE_PENDING = 5 /* internal only, never on the wire */
} raft_client_write_status_t;

typedef struct {
    uint32_t timeout_ms;
    const uint8_t *command; /* points into original request buffer */
    size_t command_len;
} raft_client_write_request_t;

typedef struct {
    raft_term_t term;
    raft_index_t index;
    raft_client_write_status_t status;
} raft_client_write_response_t;

int raft_client_write_valid_command(const uint8_t *command, size_t length);
int raft_client_write_encode_request(uint32_t timeout_ms,
                                     const uint8_t *command, size_t length,
                                     uint8_t *output, size_t capacity,
                                     size_t *output_length);
int raft_client_write_decode_request(const uint8_t *bytes, size_t length,
                                     raft_client_write_request_t *request);
int raft_client_write_encode_response(const raft_client_write_response_t *response,
                                      uint8_t output[RAFT_CLIENT_WRITE_RESPONSE_BYTES]);
int raft_client_write_decode_response(const uint8_t *bytes, size_t length,
                                      raft_client_write_response_t *response);
/* Pure leader-state check. Never reports COMMITTED solely on log admission. */
raft_client_write_status_t raft_client_write_check(const raft_node_t *node,
                                                    raft_term_t original_term,
                                                    raft_index_t original_index);
const char *raft_client_write_status_name(raft_client_write_status_t status);

#endif
