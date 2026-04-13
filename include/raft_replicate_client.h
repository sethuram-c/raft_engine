#ifndef RAFT_REPLICATE_CLIENT_H
#define RAFT_REPLICATE_CLIENT_H
#include "raft_config.h"
#include "raft_types.h"
#include <stddef.h>
#include <stdint.h>

/* Developer-only, one-shot replication probe; not a real client proposal API. */
int raft_replicate_client_send(const raft_cluster_config_t *config,
                               raft_node_id_t peer_id, raft_term_t term,
                               raft_index_t prev_index, raft_term_t prev_term,
                               const uint8_t *command, size_t len);
int raft_replicate_client_demo_propose(const raft_cluster_config_t *config,
                                       raft_node_id_t peer_id,
                                       const uint8_t *command, size_t len);
int raft_replicate_client_log_status(const raft_cluster_config_t *config,
                                    raft_node_id_t peer_id);
#endif
