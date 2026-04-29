#ifndef RAFT_HEALTH_H
#define RAFT_HEALTH_H

#include <stddef.h>
#include <stdint.h>

/* Phase 29: observational health, never a lease or a consensus proof.
 * A leader is write-ready only after a recent successful same-term
 * AppendEntries acknowledgement from at least one other voter in the
 * fixed three-member cluster. All time values use CLOCK_MONOTONIC ms.
 */
#define RAFT_HEALTH_FRESH_MS UINT64_C(3000)
#define RAFT_HEALTH_WIRE_BYTES 52u
#define RAFT_HEALTH_ELECTION_ENABLED 0x01u
#define RAFT_HEALTH_STORAGE_OK       0x02u
#define RAFT_HEALTH_QUORUM_RECENT    0x04u
#define RAFT_HEALTH_WRITE_READY      0x08u
#define RAFT_HEALTH_FLAGS_MASK       0x0fu

typedef struct {
    uint32_t node_id;
    uint8_t role; /* 0=follower, 1=candidate, 2=leader */
    uint64_t term;
    uint64_t commit_index;
    uint64_t last_applied;
    uint64_t started_ms;
    uint64_t now_ms;
    uint64_t last_ack_ms;
    uint64_t last_ack_term;
    int ack_valid;
    int election_enabled;
    int storage_healthy;
} raft_health_input_t;

typedef struct {
    uint32_t node_id;
    uint8_t role;
    uint8_t flags;
    uint64_t term;
    uint64_t commit_index;
    uint64_t last_applied;
    uint64_t quorum_age_ms; /* UINT64_MAX if no same-term quorum observation */
    uint64_t uptime_ms;
} raft_health_sample_t;

/* Failure does not modify the destination. */
int raft_health_evaluate(const raft_health_input_t *input, raft_health_sample_t *out);
int raft_health_encode(const raft_health_sample_t *sample, uint8_t *bytes, size_t size);
int raft_health_decode(const uint8_t *bytes, size_t size, raft_health_sample_t *out);
int raft_health_write_ready(const raft_health_sample_t *sample);

#endif
