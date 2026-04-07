#define _POSIX_C_SOURCE 200809L
#include "raft_timer.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

raft_status_t raft_clock_monotonic_ms(uint64_t *now_ms)
{
    if (now_ms == NULL) return RAFT_ERR_INVALID_ARGUMENT;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || ts.tv_sec < 0 ||
        ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L) return RAFT_ERR_OUT_OF_RANGE;
    /* Protect multiplication even on a platform with large time_t. */
    if ((uint64_t)ts.tv_sec > (UINT64_MAX - 999U) / 1000U)
        return RAFT_ERR_OUT_OF_RANGE;
    *now_ms = (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
    return RAFT_OK;
}

/* xorshift32 is a tiny non-cryptographic PRNG with repeatable test output. */
static uint32_t next_random(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

raft_status_t raft_election_timer_init(raft_election_timer_t *timer,
                                      uint32_t min_ms, uint32_t max_ms,
                                      uint32_t seed)
{
    if (timer == NULL || min_ms == 0 || max_ms < min_ms || seed == 0)
        return RAFT_ERR_INVALID_ARGUMENT;
    memset(timer, 0, sizeof(*timer));
    timer->min_ms = min_ms;
    timer->max_ms = max_ms;
    timer->rng_state = seed;
    return RAFT_OK;
}

raft_status_t raft_election_timer_reset(raft_election_timer_t *timer,
                                       uint64_t now_ms)
{
    if (timer == NULL || timer->rng_state == 0 || timer->min_ms == 0 ||
        timer->max_ms < timer->min_ms) return RAFT_ERR_INVALID_ARGUMENT;
    /* Every valid inclusive 32-bit interval fits in uint64_t. */
    uint64_t choices = (uint64_t)timer->max_ms - timer->min_ms + 1U;
    uint32_t selected = timer->min_ms + (uint32_t)(next_random(&timer->rng_state) % choices);
    if (now_ms > UINT64_MAX - selected) return RAFT_ERR_OUT_OF_RANGE;
    timer->timeout_ms = selected;
    timer->deadline_ms = now_ms + selected;
    timer->armed = 1;
    return RAFT_OK;
}

int raft_election_timer_expired(const raft_election_timer_t *timer,
                               uint64_t now_ms)
{
    return timer != NULL && timer->armed && now_ms >= timer->deadline_ms;
}

uint64_t raft_election_timer_remaining_ms(const raft_election_timer_t *timer,
                                          uint64_t now_ms)
{
    if (timer == NULL || !timer->armed || now_ms >= timer->deadline_ms) return 0;
    return timer->deadline_ms - now_ms;
}
