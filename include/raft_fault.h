#ifndef RAFT_FAULT_H
#define RAFT_FAULT_H

#include "raft_config.h"
#include <stddef.h>

/* PHASE 30 TEST-ONLY: outgoing Raft RPC fault injection. Never deploy this
 * mechanism on an untrusted machine. No effect unless --elect-fault is used.
 * The lab writes node-ID.block files atomically (rename) in this directory. */
#define RAFT_FAULT_DIRECTORY_MAX 320u
#define RAFT_FAULT_RULE_MAX 128u

typedef struct {
    int enabled;
    raft_node_id_t self_id;
    char directory[RAFT_FAULT_DIRECTORY_MAX];
} raft_fault_policy_t;

/* Directory must be private and owned by the current user. */
int raft_fault_init(raft_fault_policy_t *policy, raft_node_id_t self_id,
                    const char *directory);

/* Returns 1 for a blocked outgoing peer RPC, 0 for permitted. Missing rule
 * file means allow. Invalid/symlink/oversize rules FAIL CLOSED (block).
 * Does not block diagnostics/client connections or inbound peer RPCs. */
int raft_fault_blocks(const raft_fault_policy_t *policy, raft_node_id_t peer_id);

#endif
