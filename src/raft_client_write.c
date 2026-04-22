#include "raft_client_write.h"
#include "raft_state_machine.h"
#include <string.h>

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; ++i)
        p[i] = (uint8_t)(v >> (56u - 8u * i));
}

static uint64_t get64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

int raft_client_write_valid_command(const uint8_t *c, size_t n)
{
    if (c == NULL || n < 5 || n > RAFT_CLIENT_WRITE_MAX_COMMAND) return 0;
    if (memcmp(c, "DEL ", 4) == 0)
        return raft_kv_valid_key(c + 4, n - 4);
    if (memcmp(c, "SET ", 4) != 0) return 0;
    size_t end = 4;
    while (end < n && c[end] != ' ') ++end;
    return end < n && raft_kv_valid_key(c + 4, end - 4) &&
           n - end - 1 <= RAFT_KV_MAX_VALUE_BYTES;
}

int raft_client_write_encode_request(uint32_t timeout_ms,
                                     const uint8_t *command, size_t length,
                                     uint8_t *output, size_t capacity,
                                     size_t *output_length)
{
    if (output == NULL || output_length == NULL ||
        timeout_ms < RAFT_CLIENT_WRITE_MIN_TIMEOUT_MS ||
        timeout_ms > RAFT_CLIENT_WRITE_MAX_TIMEOUT_MS ||
        !raft_client_write_valid_command(command, length) ||
        capacity < 4 + length) return -1;
    put32(output, timeout_ms);
    memcpy(output + 4, command, length);
    *output_length = length + 4;
    return 0;
}

int raft_client_write_decode_request(const uint8_t *bytes, size_t length,
                                     raft_client_write_request_t *request)
{
    if (bytes == NULL || request == NULL || length < 5 ||
        length > RAFT_CLIENT_WRITE_MAX_COMMAND + 4) return -1;
    uint32_t timeout = get32(bytes);
    if (timeout < RAFT_CLIENT_WRITE_MIN_TIMEOUT_MS ||
        timeout > RAFT_CLIENT_WRITE_MAX_TIMEOUT_MS ||
        !raft_client_write_valid_command(bytes + 4, length - 4)) return -1;
    *request = (raft_client_write_request_t){timeout, bytes + 4, length - 4};
    return 0;
}

int raft_client_write_encode_response(const raft_client_write_response_t *response,
                                      uint8_t output[RAFT_CLIENT_WRITE_RESPONSE_BYTES])
{
    if (response == NULL || output == NULL ||
        response->status > RAFT_CLIENT_WRITE_UNKNOWN ||
        (response->status == RAFT_CLIENT_WRITE_COMMITTED &&
         (response->term == 0 || response->index == 0))) return -1;
    put64(output, response->term);
    put64(output + 8, response->index);
    output[16] = (uint8_t)response->status;
    return 0;
}

int raft_client_write_decode_response(const uint8_t *bytes, size_t length,
                                      raft_client_write_response_t *response)
{
    if (bytes == NULL || response == NULL ||
        length != RAFT_CLIENT_WRITE_RESPONSE_BYTES || bytes[16] >
        RAFT_CLIENT_WRITE_UNKNOWN) return -1;
    raft_client_write_response_t tmp = {get64(bytes), get64(bytes + 8),
                                      (raft_client_write_status_t)bytes[16]};
    if (tmp.status == RAFT_CLIENT_WRITE_COMMITTED &&
        (tmp.term == 0 || tmp.index == 0)) return -1;
    *response = tmp;
    return 0;
}

raft_client_write_status_t raft_client_write_check(const raft_node_t *node,
                                                    raft_term_t original_term,
                                                    raft_index_t original_index)
{
    if (node == NULL || original_term == 0 || original_index == 0 ||
        !raft_node_is_valid(node)) return RAFT_CLIENT_WRITE_UNKNOWN;
    if (node->role != RAFT_LEADER || node->current_term != original_term)
        return RAFT_CLIENT_WRITE_UNKNOWN; /* cannot prove non-commit */
    if (original_index <= node->log.base_index)
        return node->commit_index >= original_index ?
            RAFT_CLIENT_WRITE_COMMITTED : RAFT_CLIENT_WRITE_UNKNOWN;
    raft_term_t term = 0;
    if (raft_log_term_at(&node->log, original_index, &term) != RAFT_OK ||
        term != original_term) return RAFT_CLIENT_WRITE_UNKNOWN;
    return node->commit_index >= original_index ? RAFT_CLIENT_WRITE_COMMITTED :
                                                  RAFT_CLIENT_WRITE_PENDING;
}

const char *raft_client_write_status_name(raft_client_write_status_t status)
{
    switch (status) {
    case RAFT_CLIENT_WRITE_COMMITTED: return "COMMITTED";
    case RAFT_CLIENT_WRITE_NOT_LEADER: return "NOT_LEADER";
    case RAFT_CLIENT_WRITE_TIMEOUT: return "TIMEOUT";
    case RAFT_CLIENT_WRITE_REJECTED: return "REJECTED";
    case RAFT_CLIENT_WRITE_UNKNOWN: return "UNKNOWN";
    case RAFT_CLIENT_WRITE_PENDING: return "PENDING";
    default: return "INVALID";
    }
}
