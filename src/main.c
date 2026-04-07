#include "raft_config.h"
#include "raft_node.h"
#include "raft_tcp.h"
#include "raft_wire.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/time.h>

static int demo(void)
{
    raft_node_t node;
    if (raft_node_init(&node, 1) != RAFT_OK) return 1;
    const char *a = "SET x 1";
    const char *b = "SET y 2";
    node.current_term = 2;
    if (raft_log_append(&node.log, 1, a, strlen(a)) != RAFT_OK ||
        raft_log_append(&node.log, 2, b, strlen(b)) != RAFT_OK) {
        raft_node_destroy(&node);
        return 1;
    }
    printf("Demo: node=%u state=follower term=%llu entries=%zu commit=%llu applied=%llu\n",
           node.id, (unsigned long long)node.current_term, node.log.count,
           (unsigned long long)node.commit_index,
           (unsigned long long)node.last_applied);
    puts("Demo only: no elections, consensus, or log replication yet.");
    raft_node_destroy(&node);
    return 0;
}

static int load_config(const char *path, raft_cluster_config_t *config)
{
    char error[256];
    raft_config_status_t status =
        raft_config_load_file(path, config, error, sizeof(error));
    if (status != RAFT_CONFIG_OK) {
        fprintf(stderr, "Configuration %s: %s\n",
                raft_config_status_name(status), error);
        return -1;
    }
    return 0;
}

static int check_config(const char *path)
{
    raft_cluster_config_t config;
    if (load_config(path, &config) != 0) return 1;
    printf("Config valid: node_id=%u peers=%zu\n", config.node_id, config.peer_count);
    for (size_t i = 0; i < config.peer_count; ++i) {
        const raft_peer_config_t *peer = &config.peers[i];
        printf("peer %u %s:%u%s\n", peer->id, peer->ipv4,
            (unsigned int)peer->port, peer->id == config.node_id ? " (self)" : "");
    }
    puts("Config only: no sockets opened or nodes started.");
    return 0;
}

static int serve(const char *path, unsigned int max_connections)
{
    raft_cluster_config_t config;
    if (load_config(path, &config) != 0) return 1;
    char error[256];
    if (raft_tcp_serve(&config, max_connections, error, sizeof(error)) != 0) {
        fprintf(stderr, "TCP server error: %s\n", error);
        return 1;
    }
    return 0;
}

static int probe(const char *path, const char *peer_id_text)
{
    raft_cluster_config_t config;
    if (load_config(path, &config) != 0) return 1;
    if (peer_id_text == NULL || *peer_id_text == '\0' || *peer_id_text == '-') {
        fputs("Invalid peer ID: expected positive decimal integer\n", stderr);
        return 1;
    }
    errno = 0;
    char *end = NULL;
    unsigned long id = strtoul(peer_id_text, &end, 10);
    if (errno == ERANGE || end == peer_id_text || *end != '\0' ||
        id == 0 || id > UINT32_MAX) {
        fputs("Invalid peer ID: expected 1..4294967295\n", stderr);
        return 1;
    }
    const raft_peer_config_t *peer =
        raft_config_peer_for_id(&config, (raft_node_id_t)id);
    if (peer == NULL) {
        fprintf(stderr, "Peer ID %lu is not in the cluster\n", id);
        return 1;
    }
    char error[256];
    int fd = raft_tcp_connect(peer, RAFT_TCP_CONNECT_TIMEOUT_MS,
                              error, sizeof(error));
    if (fd < 0) {
        fprintf(stderr, "TCP probe failed: %s\n", error);
        return 1;
    }
    /* Keep the file-descriptor lifecycle inside the TCP module in future.
     * The CLI closes it for now after the connection-only probe. */
    (void)close(fd);
    printf("Connected: local_node=%u peer=%u %s:%u\n", config.node_id,
           peer->id, peer->ipv4, (unsigned)peer->port);
    puts("Probe only: no Raft messages sent.");
    return 0;
}

static int ping_peer(const char *path, const char *peer_id_text,
                     const char *message)
{
    raft_cluster_config_t config;
    if (load_config(path, &config) != 0) return 1;
    if (peer_id_text == NULL || *peer_id_text == '\0' || *peer_id_text == '-') {
        fputs("Invalid peer ID\n", stderr);
        return 1;
    }
    errno = 0;
    char *end = NULL;
    unsigned long id = strtoul(peer_id_text, &end, 10);
    if (errno == ERANGE || end == peer_id_text || *end != '\0' ||
        id == 0 || id > UINT32_MAX) {
        fputs("Invalid peer ID\n", stderr);
        return 1;
    }
    const raft_peer_config_t *peer =
        raft_config_peer_for_id(&config, (raft_node_id_t)id);
    if (peer == NULL) {
        fprintf(stderr, "Peer %lu not configured\n", id);
        return 1;
    }
    size_t message_len = strlen(message);
    if (message_len > RAFT_WIRE_MAX_PAYLOAD) {
        fprintf(stderr, "Message exceeds %u-byte limit\n", RAFT_WIRE_MAX_PAYLOAD);
        return 1;
    }
    char error[256];
    int fd = raft_tcp_connect(peer, RAFT_TCP_CONNECT_TIMEOUT_MS,
                              error, sizeof(error));
    if (fd < 0) {
        fprintf(stderr, "TCP ping connect failed: %s\n", error);
        return 1;
    }
    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        fprintf(stderr, "TCP ping timeout setup failed: %s\n", strerror(errno));
        (void)close(fd);
        return 1;
    }
    uint8_t response[RAFT_WIRE_MAX_PAYLOAD];
    raft_wire_type_t type = RAFT_WIRE_PING;
    size_t response_len = 0;
    raft_wire_status_t status = raft_wire_send(fd, RAFT_WIRE_PING,
                                               message, message_len,
                                               error, sizeof(error));
    if (status == RAFT_WIRE_OK)
        status = raft_wire_recv(fd, &type, response, sizeof(response),
                                &response_len, error, sizeof(error));
    (void)close(fd);
    if (status != RAFT_WIRE_OK || type != RAFT_WIRE_PONG ||
        response_len != message_len ||
        memcmp(response, message, message_len) != 0) {
        fprintf(stderr, "TCP ping failed: %s\n",
                status == RAFT_WIRE_OK ? "PONG mismatch" : error);
        return 1;
    }
    printf("PONG: local_node=%u peer=%u bytes=%zu message=", config.node_id,
           peer->id, response_len);
    (void)fwrite(response, 1, response_len, stdout);
    putchar('\n');
    puts("Demo only: no Raft RPC or consensus performed.");
    return 0;
}

/* Logical-time example: no waiting, network traffic, voting, or role change. */
static int timer_demo(const char *path)
{
    raft_cluster_config_t config;
    if (load_config(path, &config) != 0) return 1;
    raft_node_t node;
    if (raft_node_init(&node, config.node_id) != RAFT_OK) return 1;
    uint64_t now_ms;
    if (raft_clock_monotonic_ms(&now_ms) != RAFT_OK ||
        raft_node_start_follower(&node, now_ms,
                                 0x9E3779B9U ^ config.node_id) != RAFT_OK) {
        raft_node_destroy(&node);
        fputs("Could not initialize follower election timer\n", stderr);
        return 1;
    }
    uint64_t deadline = node.election_timer.deadline_ms;
    printf("Follower timer: node=%u role=%s timeout_ms=%u\n",
           node.id, raft_role_name(node.role), node.election_timer.timeout_ms);
    printf("At deadline-1: timeout_pending=%d\n",
           raft_node_follower_timeout_pending(&node, deadline - 1));
    printf("At deadline: timeout_pending=%d\n",
           raft_node_follower_timeout_pending(&node, deadline));
    if (raft_node_reset_follower_timeout(&node, deadline) != RAFT_OK) {
        raft_node_destroy(&node);
        return 1;
    }
    printf("After reset: timeout_pending=%d next_timeout_ms=%u\n",
           raft_node_follower_timeout_pending(&node, deadline),
           node.election_timer.timeout_ms);
    puts("Demo only: no candidate transition, voting, or leader election yet.");
    raft_node_destroy(&node);
    return 0;
}

static void usage(void)
{
    fputs("Usage: raft-engine [--version | --demo | --check-config PATH | "
          "--serve PATH [--max-connections N] | --probe PATH PEER_ID | "
          "--ping PATH PEER_ID MESSAGE | --timer-demo PATH]\n", stderr);
}

int main(int argc, char **argv)
{
    if (argc == 1) {
        puts("Raft Engine initialized successfully.");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts("raft-engine 0.1.0");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--demo") == 0) return demo();
    if (argc == 3 && strcmp(argv[1], "--timer-demo") == 0)
        return timer_demo(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--check-config") == 0)
        return check_config(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--serve") == 0)
        return serve(argv[2], 0);
    if (argc == 5 && strcmp(argv[1], "--serve") == 0 &&
        strcmp(argv[3], "--max-connections") == 0) {
        errno = 0;
        char *end = NULL;
        unsigned long count = strtoul(argv[4], &end, 10);
        if (argv[4][0] == '\0' || argv[4][0] == '-' || errno == ERANGE ||
            end == argv[4] || *end != '\0' || count == 0 || count > UINT_MAX) {
            fputs("Invalid --max-connections: expected positive integer\n", stderr);
            return 1;
        }
        return serve(argv[2], (unsigned int)count);
    }
    if (argc == 4 && strcmp(argv[1], "--probe") == 0)
        return probe(argv[2], argv[3]);
    if (argc == 5 && strcmp(argv[1], "--ping") == 0)
        return ping_peer(argv[2], argv[3], argv[4]);
    usage();
    return 1;
}
