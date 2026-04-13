#include "raft_node.h"
#include <stdlib.h>
#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)
#include <stdio.h>
#include <string.h>

static void test_fresh_node(void)
{
    raft_node_t node;
    CHECK(raft_node_init(&node, 7) == RAFT_OK);
    CHECK(node.role == RAFT_FOLLOWER && node.current_term == 0);
    CHECK(node.voted_for == RAFT_NO_VOTE && raft_node_is_valid(&node));
    raft_node_destroy(&node);
    puts("PASS node: valid follower initialization");
}

static void test_bad_inputs(void)
{
    raft_node_t node;
    CHECK(raft_node_init(NULL, 1) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_node_init(&node, 0) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(!raft_node_is_valid(NULL));
    CHECK(strcmp(raft_role_name((raft_role_t)123), "unknown") == 0);
    puts("PASS node: invalid inputs");
}

static void test_progress_limits(void)
{
    raft_node_t node;
    CHECK(raft_node_init(&node, 1) == RAFT_OK);
    node.current_term = 2;
    CHECK(raft_log_append(&node.log, 1, "a", 1) == RAFT_OK);
    node.commit_index = 2;
    CHECK(!raft_node_is_valid(&node));
    node.commit_index = 1;
    node.last_applied = 2;
    CHECK(!raft_node_is_valid(&node));
    node.last_applied = 1;
    CHECK(raft_node_is_valid(&node));
    raft_node_destroy(&node);
    puts("PASS node: apply/commit bounds");
}

static void test_log_terms_and_roles(void)
{
    raft_node_t node;
    CHECK(raft_node_init(&node, 1) == RAFT_OK);
    node.current_term = 1;
    CHECK(raft_log_append(&node.log, 2, "a", 1) == RAFT_OK);
    CHECK(!raft_node_is_valid(&node));
    node.current_term = 2;
    node.role = (raft_role_t)99;
    CHECK(!raft_node_is_valid(&node));
    node.role = RAFT_CANDIDATE;
    CHECK(raft_node_is_valid(&node));
    CHECK(strcmp(raft_role_name(node.role), "candidate") == 0);
    CHECK(strcmp(raft_role_name(RAFT_FOLLOWER), "follower") == 0);
    CHECK(strcmp(raft_role_name(RAFT_LEADER), "leader") == 0);
    raft_node_destroy(&node);
    puts("PASS node: role and term invariants");
}

int main(void)
{
    test_fresh_node();
    test_bad_inputs();
    test_progress_limits();
    test_log_terms_and_roles();
    puts("Node unit tests passed (4).");
    return 0;
}
