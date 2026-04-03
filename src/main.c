#include "raft_config.h"
#include "raft_node.h"

#include <stdio.h>
#include <string.h>

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
    node.commit_index = 1;
    node.last_applied = 1;
    if (!raft_node_is_valid(&node)) {
        raft_node_destroy(&node);
        return 1;
    }
    printf("Node: id=%u role=%s term=%llu\n", node.id,
        raft_role_name(node.role), (unsigned long long)node.current_term);
    printf("Log: entries=%zu last_index=%llu\n", node.log.count,
        (unsigned long long)raft_log_last_index(&node.log));
    printf("Progress: committed=%llu applied=%llu\n",
        (unsigned long long)node.commit_index,
        (unsigned long long)node.last_applied);
    puts("Demo only: no network, elections, or consensus yet.");
    raft_node_destroy(&node);
    return 0;
}

static int check_config(const char *path)
{
    raft_cluster_config_t config;
    char error[256];
    raft_config_status_t status = raft_config_load_file(path, &config, error, sizeof(error));
    if (status != RAFT_CONFIG_OK) {
        fprintf(stderr, "Configuration %s: %s\n", raft_config_status_name(status), error);
        return 1;
    }
    printf("Config valid: node_id=%u peers=%zu\n", config.node_id, config.peer_count);
    for (size_t i = 0; i < config.peer_count; ++i) {
        const raft_peer_config_t *peer = &config.peers[i];
        printf("peer %u %s:%u%s\n", peer->id, peer->ipv4,
            (unsigned int)peer->port, peer->id == config.node_id ? " (self)" : "");
    }
    puts("Config only: no sockets opened or nodes started.");
    return 0;
}

static void usage(void)
{
    fputs("Usage: raft-engine [--version | --demo | --check-config PATH]\n", stderr);
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
    usage();
    return 1;
}
