#ifndef RAFT_STATE_MACHINE_H
#define RAFT_STATE_MACHINE_H

#include "raft_node.h"
#include <stddef.h>
#include <stdint.h>

/* Deliberately small, deterministic, in-memory teaching state machine.
 * SET <key> <binary-value> and DEL <key>; keys are 1..64 ASCII bytes in
 * [A-Za-z0-9_.-], values have 0..1024 arbitrary bytes. Exactly one ASCII
 * space separates the verb and key (and key and value for SET).
 * Empty / malformed / unsupported entries and table-full inserts are
 * deterministic no-ops: they STILL consume one committed log index.
 * Allocation failure is recoverable and does NOT advance last_applied.
 * Client reads below are local diagnostics, NOT linearizable Raft reads.
 * Every function is externally synchronized with its raft_node_t. */
#define RAFT_KV_MAX_KEYS 128u
#define RAFT_KV_MAX_SESSIONS 32u
#define RAFT_KV_MAX_KEY_BYTES 64u
#define RAFT_KV_MAX_VALUE_BYTES 1024u

typedef struct {
    char key[RAFT_KV_MAX_KEY_BYTES + 1u];
    uint8_t *value;
    size_t value_len;
} raft_kv_item_t;

/* Last committed request of each client; replicated deterministic state.
 * Requests are sequential per client. Never evict a record silently. */
typedef struct {
    uint64_t client_id;
    uint64_t request_id;
    raft_index_t index;
    raft_term_t term;
    uint16_t command_len;
    uint8_t command[RAFT_KV_MAX_VALUE_BYTES];
} raft_kv_session_t;

typedef struct {
    raft_kv_session_t sessions[RAFT_KV_MAX_SESSIONS];
    size_t session_count;
    raft_kv_item_t items[RAFT_KV_MAX_KEYS];
    size_t count;
    raft_index_t executed_commands;
    raft_index_t ignored_commands;
} raft_kv_store_t;

void raft_kv_init(raft_kv_store_t *store);
void raft_kv_destroy(raft_kv_store_t *store);
int raft_kv_valid_key(const uint8_t *key, size_t key_len);
/* Pointer remains valid only until next store mutation. Absent key returns 0. */
int raft_kv_get(const raft_kv_store_t *store, const uint8_t *key, size_t key_len,
                const uint8_t **value, size_t *value_len);

/* Applies each committed, unapplied entry ONCE, in increasing index order.
 * Node->last_applied advances only AFTER successful processing. On OOM,
 * processing stops at that entry, retains all prior successful effects,
 * and the caller can safely retry later. out_applied counts progress made
 * in this call, excluding any already-applied entries. */
raft_status_t raft_apply_committed(raft_node_t *node, raft_kv_store_t *store,
                                   raft_index_t *out_applied);

#endif
