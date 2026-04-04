#include "raft_config.h"
#include "raft_node.h"
#include "raft_tcp.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

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

static void usage(void)
{
    fputs("Usage: raft-engine [--version | --demo | --check-config PATH | "
          "--serve PATH [--max-connections N] | --probe PATH PEER_ID]\n", stderr);
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
    usage();
    return 1;
}
