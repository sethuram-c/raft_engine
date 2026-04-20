#ifndef RAFT_INSTALL_H
#define RAFT_INSTALL_H

#include "raft_snapshot.h"
#include "raft_wire.h"

/* Phase 20 educational InstallSnapshot: one chunk in one framed TCP RPC.
 * Network byte order; chunk requests use an exact 40-byte header:
 * term:u64, leader_id:u32, last_index:u64, last_term:u64,
 * offset:u32, total:u32, done:u8, reserved:u8=0, length:u16, bytes[].
 * Replies: term:u64, accepted:u8, next_offset:u32.
 * A transfer is bounded by RAFT_SNAPSHOT_MAX_BYTES, which includes CRC32.
 * This is an in-memory extension, NOT crash-safe InstallSnapshot. */
#define RAFT_INSTALL_HEADER_BYTES 40u
#define RAFT_INSTALL_RESPONSE_BYTES 13u
#define RAFT_INSTALL_CHUNK_BYTES (RAFT_WIRE_MAX_PAYLOAD - RAFT_INSTALL_HEADER_BYTES)

typedef struct {
    raft_term_t term;
    raft_node_id_t leader_id;
    raft_index_t last_index;
    raft_term_t last_term;
    uint32_t offset;
    uint32_t total;
    int done;
    const uint8_t *data;
    size_t data_len;
} raft_install_chunk_t;

typedef struct {
    raft_term_t term;
    int accepted;
    uint32_t next_offset;
} raft_install_response_t;

typedef struct {
    uint8_t *bytes;
    uint32_t received;
    uint32_t total;
    raft_term_t term;
    raft_node_id_t leader_id;
    raft_index_t last_index;
    raft_term_t last_term;
} raft_install_receiver_t;

void raft_install_receiver_init(raft_install_receiver_t *receiver);
void raft_install_receiver_destroy(raft_install_receiver_t *receiver);
raft_status_t raft_install_encode_chunk(const raft_install_chunk_t *chunk,
                                        uint8_t *out, size_t capacity, size_t *length);
raft_status_t raft_install_decode_chunk(const uint8_t *bytes, size_t length,
                                        raft_install_chunk_t *chunk);
raft_status_t raft_install_encode_response(const raft_install_response_t *reply,
                                           uint8_t out[RAFT_INSTALL_RESPONSE_BYTES]);
raft_status_t raft_install_decode_response(const uint8_t *bytes, size_t length,
                                           raft_install_response_t *reply);
/* Caller ensures cluster membership and holds its node lock.
 * Returns RAFT_OK for well-formed requests, including retry/rejection replies.
 * On completion returns an encoded snapshot in completed_bytes (owned by
 * caller), and installs KV+log state without applying uncommitted entries.
 * A higher term steps down and resets the follower timer. */
raft_status_t raft_install_receive(raft_install_receiver_t *receiver,
                                   raft_node_t *node, raft_kv_store_t *store,
                                   const raft_install_chunk_t *chunk,
                                   uint64_t now_ms, raft_install_response_t *reply,
                                   uint8_t **completed_bytes, size_t *completed_length);
#endif
