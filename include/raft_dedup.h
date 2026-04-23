#ifndef RAFT_DEDUP_H
#define RAFT_DEDUP_H
#include "raft_client_write.h"
#include <stdint.h>
#include <stddef.h>

/* Phase 23: optional request-ID write API. IDs are nonzero unsigned 64-bit.
 * One outstanding request per client; request IDs strictly increase.
 * Retain the same ID and identical command for every retry.
 * Logged command: "RID1" || client:u64 || request:u64 || raw SET/DEL.
 * Wire payload: timeout:u32 || client:u64 || request:u64 || raw SET/DEL.
 * Legacy --write and its protocol stay unchanged. */
#define RAFT_DEDUP_PREFIX 20u
#define RAFT_DEDUP_WIRE_PREFIX 20u
#define RAFT_DEDUP_LOG_MAGIC "RID1"

typedef struct {
    uint64_t client_id, request_id;
    const uint8_t *command;
    size_t command_len;
} raft_dedup_entry_t;

int raft_dedup_wrap(uint64_t client_id, uint64_t request_id,
                    const uint8_t *command, size_t length,
                    uint8_t *output, size_t capacity, size_t *out_length);
/* A non-wrapped entry returns 0; a valid wrapper returns 1; invalid wrapper -1. */
int raft_dedup_parse(const uint8_t *data, size_t length, raft_dedup_entry_t *out);
int raft_dedup_encode_request(uint32_t timeout_ms, uint64_t client_id,
                              uint64_t request_id, const uint8_t *command,
                              size_t length, uint8_t *out, size_t capacity,
                              size_t *out_length);
int raft_dedup_decode_request(const uint8_t *data, size_t length,
                              uint32_t *timeout_ms, raft_dedup_entry_t *out);
#endif
