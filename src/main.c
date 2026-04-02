#include "raft_node.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static int run_demo(void)
{
    raft_node_t node;
    if (raft_node_init(&node, 1) != RAFT_OK) {
        fputs("Could not initialize demo node\n", stderr);
        return 1;
    }

    node.current_term = 2;
    const char first[] = "SET color blue";
    const char second[] = "SET mode safe";
    int result = 0;

    if (raft_log_append(&node.log, 1, first, sizeof(first) - 1) != RAFT_OK ||
        raft_log_append(&node.log, 2, second, sizeof(second) - 1) != RAFT_OK) {
        fputs("Could not append demo log entries\n", stderr);
        result = 1;
    } else {
        node.commit_index = 1;
        node.last_applied = 1;
        if (!raft_node_is_valid(&node)) {
            fputs("Demo node invariants failed\n", stderr);
            result = 1;
        } else {
            printf("Node: id=%" PRIu32 " role=%s term=%" PRIu64 "\n",
                   node.id, raft_role_name(node.role), node.current_term);
            printf("Log: entries=%zu last_index=%" PRIu64 "\n",
                   node.log.count, raft_log_last_index(&node.log));
            printf("Progress: committed=%" PRIu64 " applied=%" PRIu64 "\n",
                   node.commit_index, node.last_applied);
            puts("Demo only: no network, elections, or consensus yet.");
        }
    }

    raft_node_destroy(&node);
    return result;
}

int main(int argc, char *argv[])
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts("raft-engine 0.2.0");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--demo") == 0) {
        return run_demo();
    }
    if (argc != 1) {
        fprintf(stderr, "Usage: raft-engine [--version|--demo]\n");
        return 1;
    }
    puts("Raft Engine initialized successfully.");
    return 0;
}
