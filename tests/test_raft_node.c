#include "raft_node.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static void test_initial_state(void)
{
    raft_node_t node;
    CHECK(raft_node_init(&node, 1) == RAFT_OK);
    CHECK(node.id == 1);
    CHECK(node.role == RAFT_FOLLOWER);
    CHECK(node.current_term == 0);
    CHECK(node.voted_for == RAFT_NO_VOTE);
    CHECK(node.commit_index == 0);
    CHECK(node.last_applied == 0);
    CHECK(raft_log_last_index(&node.log) == 0);
    CHECK(raft_node_is_valid(&node));
    raft_node_destroy(&node);
}

static void test_invalid_initialization(void)
{
    CHECK(raft_node_init(NULL, 1) == RAFT_ERR_INVALID_ARGUMENT);
    raft_node_t node;
    CHECK(raft_node_init(&node, RAFT_NO_VOTE) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(!raft_node_is_valid(NULL));
}

static void test_node_invariants(void)
{
    raft_node_t node;
    CHECK(raft_node_init(&node, 7) == RAFT_OK);
    node.current_term = 3;
    CHECK(raft_log_append(&node.log, 2, "a", 1) == RAFT_OK);
    CHECK(raft_log_append(&node.log, 3, "b", 1) == RAFT_OK);
    node.commit_index = 1;
    node.last_applied = 1;
    CHECK(raft_node_is_valid(&node));

    node.last_applied = 2;
    CHECK(!raft_node_is_valid(&node));
    node.last_applied = 1;
    node.commit_index = 3;
    CHECK(!raft_node_is_valid(&node));
    node.commit_index = 1;
    node.current_term = 1;
    CHECK(!raft_node_is_valid(&node));
    node.current_term = 3;
    node.role = (raft_role_t)77;
    CHECK(!raft_node_is_valid(&node));
    node.role = RAFT_FOLLOWER;
    CHECK(raft_node_is_valid(&node));
    raft_node_destroy(&node);
}

static void test_role_names_and_cleanup(void)
{
    CHECK(strcmp(raft_role_name(RAFT_FOLLOWER), "follower") == 0);
    CHECK(strcmp(raft_role_name(RAFT_CANDIDATE), "candidate") == 0);
    CHECK(strcmp(raft_role_name(RAFT_LEADER), "leader") == 0);
    CHECK(strcmp(raft_role_name((raft_role_t)123), "unknown") == 0);

    raft_node_t node;
    CHECK(raft_node_init(&node, 3) == RAFT_OK);
    CHECK(raft_log_append(&node.log, 1, "cmd", 3) == RAFT_OK);
    raft_node_destroy(&node);
    CHECK(node.id == 0);
    raft_node_destroy(&node);
    raft_node_destroy(NULL);
}

int main(void)
{
    test_initial_state();
    test_invalid_initialization();
    test_node_invariants();
    test_role_names_and_cleanup();
    puts("PASS: 4 Raft node unit tests");
    return 0;
}
