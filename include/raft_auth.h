#ifndef RAFT_AUTH_H
#define RAFT_AUTH_H

#include "raft_dedup.h"
#include <stddef.h>
#include <stdint.h>

/* Phase 25: per-client HMAC-SHA-256 credentials, separate from Raft peer auth.
 * The signed request is the Phase 23 request || 32-byte MAC.
 * Domain separation also binds the frame's purpose to this protocol version.
 * Keys are 32 random bytes stored in owner-only regular files.
 */
#define RAFT_AUTH_KEY_BYTES 32u
#define RAFT_AUTH_TAG_BYTES 32u
#define RAFT_AUTH_MAX_CLIENTS 32u
#define RAFT_AUTH_DOMAIN "RAFT25-CLIENT-WRITE-V1"

typedef struct {
    uint64_t client_id;
    uint8_t key[RAFT_AUTH_KEY_BYTES];
} raft_auth_client_t;

typedef struct {
    size_t count;
    raft_auth_client_t clients[RAFT_AUTH_MAX_CLIENTS];
} raft_auth_keys_t;

/* owner-only regular files, reject symlinks, extra bytes and duplicate IDs.
 * A server keyring has lines `CLIENT_ID 64_HEX_DIGITS`.
 * A client secret file contains only `64_HEX_DIGITS` and newline.
 */
int raft_auth_read_keyring(const char *path, raft_auth_keys_t *out);
int raft_auth_read_secret(const char *path, uint8_t key[RAFT_AUTH_KEY_BYTES]);
void raft_auth_clear_keys(raft_auth_keys_t *keys);
void raft_auth_clear_secret(uint8_t key[RAFT_AUTH_KEY_BYTES]);

int raft_auth_sign(uint32_t timeout_ms, uint64_t client_id,
                   uint64_t request_id, const uint8_t *command, size_t command_len,
                   const uint8_t key[RAFT_AUTH_KEY_BYTES],
                   uint8_t *output, size_t capacity, size_t *output_len);
/* Fails closed, does not expose request on failure. */
int raft_auth_verify(const raft_auth_keys_t *keys, const uint8_t *payload,
                     size_t payload_len, uint32_t *timeout_ms,
                     raft_dedup_entry_t *request);
#endif
