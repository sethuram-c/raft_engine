#include "raft_replicate_client.h"
#include "raft_append.h"
#include "raft_tcp.h"
#include "raft_wire.h"
#include "raft_state_machine.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static uint64_t read_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

static int connect_peer(const raft_cluster_config_t *config,
                        raft_node_id_t peer_id, char error[256])
{
    const raft_peer_config_t *peer = raft_config_peer_for_id(config, peer_id);
    if (peer == NULL || peer_id == config->node_id) {
        snprintf(error, 256, "unknown peer or self-target");
        return -1;
    }
    int fd = raft_tcp_connect(peer, RAFT_TCP_CONNECT_TIMEOUT_MS, error, 256);
    if (fd == -1) return -1;
    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        snprintf(error, 256, "setsockopt: %s", strerror(errno));
        (void)close(fd);
        return -1;
    }
    return fd;
}

int raft_replicate_client_send(const raft_cluster_config_t *config,
                                raft_node_id_t peer_id, raft_term_t term,
                                raft_index_t prev_index, raft_term_t prev_term,
                                const uint8_t *command, size_t len)
{
    if (config == NULL || command == NULL || !len || !term ||
        (prev_index == 0) != (prev_term == 0)) return -1;
    raft_append_batch_t batch = {0};
    batch.header = (raft_append_request_t){term, config->node_id,
                                           prev_index, prev_term, 0};
    batch.entry_count = 1;
    batch.entries[0] = (raft_log_entry_t){term, (uint8_t *)command, len};
    uint8_t payload[RAFT_WIRE_MAX_PAYLOAD];
    size_t length = 0;
    if (raft_append_encode_batch(&batch, payload, sizeof(payload), &length) != RAFT_OK) {
        fputs("Invalid replication batch (limit 4096 bytes)\n", stderr);
        return -1;
    }
    char error[256];
    int fd = connect_peer(config, peer_id, error);
    if (fd < 0) { fprintf(stderr, "Connect failed: %s\n", error); return -1; }
    raft_wire_status_t status = raft_wire_send(fd, RAFT_WIRE_APPEND_ENTRIES,
                                 payload, length, error, sizeof(error));
    raft_wire_type_t response_type = 0;
    uint8_t response_data[RAFT_APPEND_RESPONSE_BYTES];
    size_t response_length = 0;
    if (status == RAFT_WIRE_OK)
        status = raft_wire_recv(fd, &response_type, response_data,
                                sizeof(response_data), &response_length,
                                error, sizeof(error));
    (void)close(fd);
    raft_append_response_t reply = {0};
    if (status != RAFT_WIRE_OK || response_type != RAFT_WIRE_APPEND_ENTRIES_RESPONSE ||
        raft_append_decode_response(response_data, response_length, &reply) != RAFT_OK) {
        fprintf(stderr, "Replication RPC failed: %s\n",
                status == RAFT_WIRE_OK ? "invalid response" : error);
        return -1;
    }
    printf("Replication reply: follower=%u term=%llu success=%d sent_entries=1\n",
           peer_id, (unsigned long long)reply.term, reply.success);
    puts("Diagnostic only: this command does not prove elected leadership or commitment.");
    return reply.success ? 0 : 2;
}

int raft_replicate_client_log_status(const raft_cluster_config_t *config,
                                     raft_node_id_t peer_id)
{
    if (config == NULL) return -1;
    char error[256];
    int fd = connect_peer(config, peer_id, error);
    if (fd < 0) { fprintf(stderr, "Connect failed: %s\n", error); return -1; }
    raft_wire_status_t status = raft_wire_send(fd, RAFT_WIRE_LOG_STATUS_REQUEST,
                                 NULL, 0, error, sizeof(error));
    raft_wire_type_t type = 0;
    uint8_t data[24];
    size_t length = 0;
    if (status == RAFT_WIRE_OK)
        status = raft_wire_recv(fd, &type, data, sizeof(data), &length,
                                error, sizeof(error));
    (void)close(fd);
    if (status != RAFT_WIRE_OK || type != RAFT_WIRE_LOG_STATUS_RESPONSE || length != 24) {
        fprintf(stderr, "Log status failed: %s\n",
                status == RAFT_WIRE_OK ? "invalid reply" : error);
        return -1;
    }
    printf("Log status: peer=%u last_index=%llu last_term=%llu commit_index=%llu\n",
           peer_id, (unsigned long long)read_u64(data),
           (unsigned long long)read_u64(data+8),
           (unsigned long long)read_u64(data+16));
    return 0;
}

/* Local diagnostic only: no authentication, no majority commitment. */
int raft_replicate_client_demo_propose(const raft_cluster_config_t *config,
                                       raft_node_id_t peer_id,
                                       const uint8_t *command, size_t len)
{
    if (config == NULL || command == NULL || len == 0 || len > 1024) return -1;
    char error[256];
    int fd = connect_peer(config, peer_id, error);
    if (fd < 0) { fprintf(stderr, "Connect failed: %s\n", error); return -1; }
    raft_wire_status_t status = raft_wire_send(fd, RAFT_WIRE_DEMO_PROPOSE,
                                  command, len, error, sizeof(error));
    raft_wire_type_t type = 0;
    uint8_t reply[17];
    size_t reply_len = 0;
    if (status == RAFT_WIRE_OK)
        status = raft_wire_recv(fd, &type, reply, sizeof(reply), &reply_len,
                                error, sizeof(error));
    (void)close(fd);
    if (status != RAFT_WIRE_OK || type != RAFT_WIRE_DEMO_PROPOSE_RESPONSE ||
        reply_len != sizeof(reply) || reply[16] > 1) {
        fprintf(stderr, "Demo proposal failed: %s\n",
                status == RAFT_WIRE_OK ? "invalid response" : error);
        return -1;
    }
    printf("Demo proposal: peer=%u term=%llu index=%llu accepted=%u (uncommitted)\n",
           peer_id, (unsigned long long)read_u64(reply),
           (unsigned long long)read_u64(reply+8), reply[16]);
    return reply[16] ? 0 : 2;
}

/* Same short-lived framed TCP connections as the other diagnostic commands. */
static int request_diagnostic(const raft_cluster_config_t *config,
                              raft_node_id_t peer_id, raft_wire_type_t request_type,
                              const void *request, size_t request_length,
                              raft_wire_type_t expected_type,
                              uint8_t *response, size_t response_capacity,
                              size_t *response_length)
{
    char error[256];
    int fd = connect_peer(config, peer_id, error);
    if (fd < 0) { fprintf(stderr, "Connect failed: %s\n", error); return -1; }
    raft_wire_status_t status = raft_wire_send(fd, request_type,
                              request, request_length, error, sizeof(error));
    raft_wire_type_t actual_type = 0;
    if (status == RAFT_WIRE_OK)
        status = raft_wire_recv(fd, &actual_type, response, response_capacity,
                                response_length, error, sizeof(error));
    (void)close(fd);
    if (status != RAFT_WIRE_OK || actual_type != expected_type) {
        fprintf(stderr, "Diagnostic request failed: %s\n",
                status == RAFT_WIRE_OK ? "wrong reply type" : error);
        return -1;
    }
    return 0;
}

int raft_replicate_client_apply_status(const raft_cluster_config_t *config,
                                       raft_node_id_t peer_id)
{
    if (config == NULL) return -1;
    uint8_t response[28];
    size_t length = 0;
    if (request_diagnostic(config, peer_id, RAFT_WIRE_APPLY_STATUS_REQUEST,
                           NULL, 0, RAFT_WIRE_APPLY_STATUS_RESPONSE,
                           response, sizeof(response), &length) != 0 ||
        length != sizeof(response)) return -1;
    uint32_t keys = ((uint32_t)response[16] << 24) |
                    ((uint32_t)response[17] << 16) |
                    ((uint32_t)response[18] << 8) | response[19];
    printf("Apply status: peer=%u last_applied=%llu commit_index=%llu keys=%u ignored=%llu\n",
           peer_id, (unsigned long long)read_u64(response),
           (unsigned long long)read_u64(response+8), keys,
           (unsigned long long)read_u64(response+20));
    return 0;
}

int raft_replicate_client_demo_get(const raft_cluster_config_t *config,
                                   raft_node_id_t peer_id,
                                   const uint8_t *key, size_t key_len)
{
    if (config == NULL || !raft_kv_valid_key(key, key_len)) return -1;
    uint8_t response[11 + RAFT_KV_MAX_VALUE_BYTES];
    size_t length = 0;
    if (request_diagnostic(config, peer_id, RAFT_WIRE_DEMO_GET_REQUEST,
                           key, key_len, RAFT_WIRE_DEMO_GET_RESPONSE,
                           response, sizeof(response), &length) != 0 ||
        length < 11 || response[0] > 1) return -1;
    size_t value_len = ((size_t)response[9] << 8) | response[10];
    if (value_len > RAFT_KV_MAX_VALUE_BYTES ||
        length != 11 + (response[0] ? value_len : 0) ||
        (!response[0] && value_len != 0)) return -1;
    printf("Local key read: peer=%u key=%.*s found=%u last_applied=%llu",
           peer_id, (int)key_len, (const char *)key, response[0],
           (unsigned long long)read_u64(response+1));
    if (response[0]) {
        printf(" value_hex=");
        for (size_t i = 0; i < value_len; ++i) printf("%02x", response[11+i]);
    }
    putchar('\n');
    puts("Diagnostic only: local read, no leader/lease or linearizability guarantee.");
    return 0;
}
