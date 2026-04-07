#include "raft_node.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_invalid_arguments(void)
{
    raft_election_timer_t t;
    assert(raft_clock_monotonic_ms(NULL) == RAFT_ERR_INVALID_ARGUMENT);
    assert(raft_election_timer_init(NULL, 10, 20, 1) == RAFT_ERR_INVALID_ARGUMENT);
    assert(raft_election_timer_init(&t, 0, 20, 1) == RAFT_ERR_INVALID_ARGUMENT);
    assert(raft_election_timer_init(&t, 40, 20, 1) == RAFT_ERR_INVALID_ARGUMENT);
    assert(raft_election_timer_init(&t, 10, 20, 0) == RAFT_ERR_INVALID_ARGUMENT);
    assert(raft_election_timer_reset(NULL, 2) == RAFT_ERR_INVALID_ARGUMENT);
    assert(!raft_election_timer_expired(NULL, UINT64_MAX));
    assert(raft_election_timer_remaining_ms(NULL, 3) == 0);
    puts("PASS timer: invalid arguments");
}

static void test_monotonic_clock(void)
{
    uint64_t a = 0, b = 0;
    assert(raft_clock_monotonic_ms(&a) == RAFT_OK);
    assert(raft_clock_monotonic_ms(&b) == RAFT_OK);
    assert(b >= a);
    puts("PASS timer: CLOCK_MONOTONIC is nondecreasing");
}

static void test_boundaries(void)
{
    raft_election_timer_t t;
    assert(raft_election_timer_init(&t, 350, 700, 123) == RAFT_OK);
    assert(!raft_election_timer_expired(&t, UINT64_MAX));
    assert(raft_election_timer_remaining_ms(&t, 0) == 0);
    for (uint64_t i = 0; i < 1000; ++i) {
        uint64_t now = i * 1000;
        assert(raft_election_timer_reset(&t, now) == RAFT_OK);
        assert(t.timeout_ms >= 350 && t.timeout_ms <= 700);
        assert(t.deadline_ms == now + t.timeout_ms);
        assert(!raft_election_timer_expired(&t, t.deadline_ms - 1));
        assert(raft_election_timer_expired(&t, t.deadline_ms));
        assert(raft_election_timer_remaining_ms(&t, now) == t.timeout_ms);
        assert(raft_election_timer_remaining_ms(&t, t.deadline_ms) == 0);
    }
    puts("PASS timer: 1000 inclusive deadline and jitter checks");
}

static void test_rearm_and_seed(void)
{
    raft_election_timer_t a, b, c;
    assert(raft_election_timer_init(&a, 10, 20, 123) == RAFT_OK);
    assert(raft_election_timer_init(&b, 10, 20, 123) == RAFT_OK);
    assert(raft_election_timer_init(&c, 10, 20, 456) == RAFT_OK);
    int differs = 0;
    for (uint64_t i = 0; i < 20; ++i) {
        assert(raft_election_timer_reset(&a, i * 100) == RAFT_OK);
        assert(raft_election_timer_reset(&b, i * 100) == RAFT_OK);
        assert(raft_election_timer_reset(&c, i * 100) == RAFT_OK);
        assert(a.deadline_ms == b.deadline_ms);
        if (a.timeout_ms != c.timeout_ms) differs = 1;
    }
    assert(differs);
    puts("PASS timer: determinism, independent seeds, repeated reset");
}

static void test_overflow_guard(void)
{
    raft_election_timer_t t;
    assert(raft_election_timer_init(&t, 100, 100, 1) == RAFT_OK);
    assert(raft_election_timer_reset(&t, UINT64_MAX - 99) == RAFT_ERR_OUT_OF_RANGE);
    assert(!t.armed);
    assert(raft_election_timer_reset(&t, UINT64_MAX - 100) == RAFT_OK);
    assert(t.deadline_ms == UINT64_MAX);
    puts("PASS timer: overflow is rejected safely");
}

static void test_follower_lifecycle(void)
{
    raft_node_t node;
    assert(raft_node_init(&node, 2) == RAFT_OK);
    assert(!raft_node_follower_timeout_pending(&node, UINT64_MAX));
    node.current_term = 4;
    node.voted_for = 2;
    assert(raft_node_start_follower(&node, 1000, 42) == RAFT_OK);
    assert(raft_node_is_valid(&node));
    assert(node.role == RAFT_FOLLOWER);
    assert(node.current_term == 4 && node.voted_for == 2);
    uint64_t first_deadline = node.election_timer.deadline_ms;
    assert(raft_node_follower_timeout_pending(&node, first_deadline));
    assert(raft_node_reset_follower_timeout(&node, first_deadline) == RAFT_OK);
    assert(node.election_timer.deadline_ms > first_deadline);
    assert(!raft_node_follower_timeout_pending(&node, first_deadline));
    assert(raft_node_follower_timeout_pending(&node, node.election_timer.deadline_ms));
    node.role = RAFT_CANDIDATE;
    assert(!raft_node_follower_timeout_pending(&node, UINT64_MAX));
    assert(raft_node_reset_follower_timeout(&node, 100) == RAFT_ERR_INVALID_ARGUMENT);
    node.role = RAFT_FOLLOWER;
    node.election_timer.timeout_ms = 0;
    assert(!raft_node_is_valid(&node));
    raft_node_destroy(&node);
    puts("PASS timer: follower role gating and no term/vote mutation");
}

static void test_follower_invalid_and_atomic(void)
{
    raft_node_t node;
    assert(raft_node_init(&node, 1) == RAFT_OK);
    assert(raft_node_start_follower(NULL, 100, 1) == RAFT_ERR_INVALID_ARGUMENT);
    assert(raft_node_start_follower(&node, 100, 0) == RAFT_ERR_INVALID_ARGUMENT);
    assert(!node.election_timer.armed);
    assert(raft_node_reset_follower_timeout(&node, 100) == RAFT_ERR_INVALID_ARGUMENT);
    assert(raft_node_start_follower(&node, UINT64_MAX, 1) == RAFT_ERR_OUT_OF_RANGE);
    assert(!node.election_timer.armed);
    raft_node_destroy(&node);
    puts("PASS timer: follower errors do not partially arm");
}

int main(void)
{
    test_invalid_arguments();
    test_monotonic_clock();
    test_boundaries();
    test_rearm_and_seed();
    test_overflow_guard();
    test_follower_lifecycle();
    test_follower_invalid_and_atomic();
    puts("Timer unit tests passed (7 groups).");
    return 0;
}
