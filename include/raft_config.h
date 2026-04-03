#ifndef RAFT_CONFIG_H
#define RAFT_CONFIG_H

#include "raft_types.h"
#include <stddef.h>
#include <stdint.h>

/* Phase 3 supports a static, exactly-three-node, IPv4-only cluster. */
#define RAFT_CLUSTER_SIZE 3
#define RAFT_IPV4_TEXT_SIZE 16

typedef struct {
    raft_node_id_t id;
    char ipv4[RAFT_IPV4_TEXT_SIZE];
    uint16_t port;
} raft_peer_config_t;

typedef struct {
    raft_node_id_t node_id;
    raft_peer_config_t peers[RAFT_CLUSTER_SIZE];
    size_t peer_count;
} raft_cluster_config_t;

typedef enum {
    RAFT_CONFIG_OK = 0,
    RAFT_CONFIG_INVALID_ARGUMENT,
    RAFT_CONFIG_IO_ERROR,
    RAFT_CONFIG_PARSE_ERROR,
    RAFT_CONFIG_VALIDATION_ERROR
} raft_config_status_t;

/* On failure, *out remains unchanged; error includes the problematic line. */
raft_config_status_t raft_config_load_file(const char *path,
    raft_cluster_config_t *out, char *error, size_t error_size);

/* Validate hand-constructed configuration too. */
raft_config_status_t raft_config_validate(const raft_cluster_config_t *config,
    char *error, size_t error_size);

/* Returned pointer is borrowed and belongs to config. */
const raft_peer_config_t *raft_config_peer_for_id(
    const raft_cluster_config_t *config, raft_node_id_t id);

const char *raft_config_status_name(raft_config_status_t status);

#endif /* RAFT_CONFIG_H */
