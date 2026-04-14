#include "raft_commit.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks = 0;
#define CHECK(e) do { ++checks; if (!(e)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #e); exit(1); \
} } while (0)

static void setup(raft_node_t *n)
{
    CHECK(raft_node_init(n, 1) == RAFT_OK);
    CHECK(raft_node_start_follower(n, 100, 9) == RAFT_OK);
    n->role = RAFT_LEADER;
    n->current_term = 5;
    for (int i = 0; i < 5; ++i) {
        raft_term_t t = i < 2 ? 1 : (i == 2 ? 3 : 5);
        CHECK(raft_log_append(&n->log, t, "x", 1) == RAFT_OK);
    }
    CHECK(raft_node_is_valid(n));
}

static void initialize(raft_replication_progress_t p[2])
{
    CHECK(raft_replication_progress_init(&p[0], 2, 5, 5) == RAFT_OK);
    CHECK(raft_replication_progress_init(&p[1], 3, 5, 5) == RAFT_OK);
}

static void confirmed(raft_replication_progress_t *p, raft_index_t index)
{
    p->match_index = index;
    p->next_index = index + 1;
}

static void test_majority_and_current_term(void)
{
    raft_node_t n;
    setup(&n);
    raft_replication_progress_t p[2];
    initialize(p);
    raft_index_t new_commit = 99;
    CHECK(raft_commit_advance(&n, p, 2, &new_commit) == RAFT_OK);
    CHECK(n.commit_index == 0 && new_commit == 0); /* Self alone != quorum. */
    confirmed(&p[0], 2);
    CHECK(raft_commit_advance(&n, p, 2, &new_commit) == RAFT_OK);
    CHECK(n.commit_index == 0); /* Majority old-term entry cannot directly commit. */
    confirmed(&p[0], 3);
    CHECK(raft_commit_advance(&n, p, 2, &new_commit) == RAFT_OK);
    CHECK(n.commit_index == 0); /* Even term 3 < leader's term 5. */
    confirmed(&p[0], 4);
    CHECK(raft_commit_advance(&n, p, 2, &new_commit) == RAFT_OK);
    CHECK(n.commit_index == 4 && new_commit == 4); /* Commits indices 1..4. */
    confirmed(&p[0], 5);
    CHECK(raft_commit_advance(&n, p, 2, &new_commit) == RAFT_OK);
    CHECK(n.commit_index == 5);
    confirmed(&p[0], 1);
    CHECK(raft_commit_advance(&n, p, 2, &new_commit) == RAFT_OK);
    CHECK(n.commit_index == 5); /* No regression, even if peer restarts. */
    raft_node_destroy(&n);
}

static void test_two_followers_and_validation(void)
{
    raft_node_t n;
    setup(&n);
    raft_replication_progress_t p[2];
    initialize(p);
    raft_index_t out = 42;
    confirmed(&p[0], 3);
    confirmed(&p[1], 4);
    CHECK(raft_commit_advance(&n, p, 2, &out) == RAFT_OK);
    CHECK(n.commit_index == 4); /* Leader and peer 3 have index 4. */
    n.commit_index = 0;
    p[1].peer_id = 2;
    CHECK(raft_commit_advance(&n, p, 2, &out) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(out == 4 && n.commit_index == 0);
    p[1].peer_id = 3;
    p[1].term = 4;
    CHECK(raft_commit_advance(&n, p, 2, &out) == RAFT_ERR_INVALID_ARGUMENT);
    p[1].term = 5;
    p[1].match_index = 6;
    CHECK(raft_commit_advance(&n, p, 2, &out) == RAFT_ERR_INVALID_ARGUMENT);
    p[1].match_index = 4;
    p[1].next_index = 4; /* Inconsistent observation. */
    CHECK(raft_commit_advance(&n, p, 2, &out) == RAFT_ERR_INVALID_ARGUMENT);
    p[1].next_index = 5;
    CHECK(raft_commit_advance(&n, p, 1, &out) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_commit_advance(NULL, p, 2, &out) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_commit_advance(&n, NULL, 2, &out) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_commit_advance(&n, p, 2, NULL) == RAFT_ERR_INVALID_ARGUMENT);
    n.role = RAFT_FOLLOWER;
    CHECK(raft_commit_advance(&n, p, 2, &out) == RAFT_ERR_INVALID_ARGUMENT);
    n.role = RAFT_LEADER;
    n.current_term = 6; /* Other term's matchIndex is stale. */
    CHECK(raft_commit_advance(&n, p, 2, &out) == RAFT_ERR_INVALID_ARGUMENT);
    raft_node_destroy(&n);
}

static void test_heartbeat_commit_propagation(void)
{
    raft_node_t leader, follower;
    setup(&leader);
    CHECK(raft_node_init(&follower, 2) == RAFT_OK);
    CHECK(raft_node_start_follower(&follower, 100, 23) == RAFT_OK);
    follower.current_term = 5;
    /* Follower has the same five entries in memory, but has yet to commit. */
    for (int i = 0; i < 5; ++i) {
        raft_term_t t = i < 2 ? 1 : (i == 2 ? 3 : 5);
        CHECK(raft_log_append(&follower.log, t, "x", 1) == RAFT_OK);
    }
    raft_replication_progress_t p[2];
    initialize(p);
    confirmed(&p[0], 5);
    raft_index_t committed = 0;
    CHECK(raft_commit_advance(&leader, p, 2, &committed) == RAFT_OK);
    CHECK(committed == 5);
    raft_append_batch_t batch;
    raft_index_t through = 0;
    CHECK(raft_replication_prepare(&leader, &p[0], &batch, &through) == RAFT_OK);
    CHECK(batch.entry_count == 0 && batch.header.leader_commit == 5);
    raft_append_response_t reply;
    CHECK(raft_append_handle_batch(&follower, &batch, 200, &reply) == RAFT_OK);
    CHECK(reply.success && follower.commit_index == 5 && follower.last_applied == 0);
    /* A brand-new follower cannot leap past a matched prefix. */
    raft_node_t lagging;
    CHECK(raft_node_init(&lagging, 3) == RAFT_OK);
    CHECK(raft_node_start_follower(&lagging, 100, 67) == RAFT_OK);
    lagging.current_term = 5;
    raft_append_batch_t empty = {.header = {5, 1, 0, 0, 5}, .entry_count = 0};
    CHECK(raft_append_handle_batch(&lagging, &empty, 200, &reply) == RAFT_OK);
    CHECK(reply.success && lagging.commit_index == 0);
    raft_node_destroy(&lagging);
    raft_node_destroy(&follower);
    raft_node_destroy(&leader);
}

int main(void)
{
    test_majority_and_current_term();
    test_two_followers_and_validation();
    test_heartbeat_commit_propagation();
    printf("PASS Phase 14: %u majority, current-term, validation, propagation assertions\n", checks);
    return 0;
}
