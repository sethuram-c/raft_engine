#ifndef RAFT_TIMER_H
#define RAFT_TIMER_H

#include "raft_types.h"
#include <stdint.h>

/* This is a deterministic, clock-injected scheduler for election timeouts.
 * It is NOT a network timer and does NOT elect a leader. The caller owns
 * serialization when the containing node is shared between threads. */
#define RAFT_ELECTION_TIMEOUT_MIN_MS 350U
#define RAFT_ELECTION_TIMEOUT_MAX_MS 700U

typedef struct {
    uint32_t min_ms;
    uint32_t max_ms;
    uint32_t timeout_ms;
    uint32_t rng_state;
    uint64_t deadline_ms;
    int armed;
} raft_election_timer_t;

/* Use CLOCK_MONOTONIC for elapsed time; this is not wall-clock time. */
raft_status_t raft_clock_monotonic_ms(uint64_t *now_ms);

/* seed must be nonzero; callers should use independent seeds per node. */
raft_status_t raft_election_timer_init(raft_election_timer_t *timer,
                                      uint32_t min_ms, uint32_t max_ms,
                                      uint32_t seed);
/* Select a new randomized deadline, inclusive of min_ms and max_ms. */
raft_status_t raft_election_timer_reset(raft_election_timer_t *timer,
                                       uint64_t now_ms);
int raft_election_timer_expired(const raft_election_timer_t *timer,
                               uint64_t now_ms);
uint64_t raft_election_timer_remaining_ms(const raft_election_timer_t *timer,
                                          uint64_t now_ms);

#endif
