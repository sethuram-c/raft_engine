#ifndef RAFT_CLIENT_AUTO_H
#define RAFT_CLIENT_AUTO_H

#include "raft_config.h"
#include "raft_client_write.h"
#include <stddef.h>
#include <stdint.h>

/* Phase 24: an opt-in, topology-aware retrying client, not an election RPC.
 * Every retry reuses the identical client/request IDs and command. */
#define RAFT_AUTO_MIN_BUDGET_MS 500u
#define RAFT_AUTO_MAX_BUDGET_MS 30000u
#define RAFT_AUTO_RETRY_DELAY_MS 80u

typedef struct {
    raft_node_id_t id;
    raft_term_t term;
    uint8_t role;
    int reachable;
} raft_auto_peer_state_t;

/* Validates a 17-byte Phase 9 STATUS reply. Does not modify out on failure. */
int raft_auto_decode_status(const uint8_t *data, size_t length,
                            raft_node_id_t expected_id,
                            raft_auto_peer_state_t *out);
/* Selects a unique reported leader at the highest observed term. No quorum
 * proof or lease is implied; the server validates leadership on every write. */
raft_node_id_t raft_auto_choose_leader(const raft_auto_peer_state_t *states,
                                        size_t count);
/* 0 committed; 4 rejected; -1 retry same identity. */
int raft_auto_write_decision(raft_client_write_status_t status);
/* Return 0 for committed, 3 for unresolved deadline, 4 for rejected, 1 for
 * bad arguments/runtime clock errors. Outputs stable, grep-friendly lines. */
int raft_client_auto_write(const raft_cluster_config_t *config,
                            uint32_t overall_timeout_ms,
                            uint64_t client_id, uint64_t request_id,
                            const uint8_t *command, size_t command_length);

#endif
