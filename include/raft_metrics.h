#ifndef RAFT_METRICS_H
#define RAFT_METRICS_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

/* Phase 28: fixed, versioned, read-only operational counters and gauges.
 * No client commands, keys, addresses, certificate fields, or request IDs.
 * Counters reset when a process restarts; they are intentionally not WAL state.
 */
typedef enum {
    RAFT_METRIC_NODE_ID,
    RAFT_METRIC_UPTIME_MS,
    RAFT_METRIC_TERM,
    RAFT_METRIC_ROLE,
    RAFT_METRIC_LAST_LOG_INDEX,
    RAFT_METRIC_COMMIT_INDEX,
    RAFT_METRIC_LAST_APPLIED,
    RAFT_METRIC_SNAPSHOT_INDEX,
    RAFT_METRIC_ELECTION_ROUNDS,
    RAFT_METRIC_LEADER_TRANSITIONS,
    RAFT_METRIC_VOTE_REQUESTS,
    RAFT_METRIC_VOTES_GRANTED,
    RAFT_METRIC_APPEND_REQUESTS,
    RAFT_METRIC_APPEND_REJECTIONS,
    RAFT_METRIC_REPLICATION_RPC_FAILURES,
    RAFT_METRIC_SNAPSHOTS_INSTALLED,
    RAFT_METRIC_CLIENT_COMMITS,
    RAFT_METRIC_CLIENT_TIMEOUTS,
    RAFT_METRIC_TLS_HANDSHAKE_FAILURES,
    RAFT_METRIC_TLS_RELOAD_SUCCESSES,
    RAFT_METRIC_TLS_RELOAD_FAILURES,
    RAFT_METRIC_CONNECTIONS_ACCEPTED,
    RAFT_METRIC_CONNECTIONS_ACTIVE,
    RAFT_METRIC_CONNECTIONS_PEAK,
    RAFT_METRIC_REJECTED_FRAMES,
    RAFT_METRIC_MAX_FOLLOWER_LAG,
    RAFT_METRIC_STORAGE_FAILURES,
    RAFT_METRIC_REPLICATION_RPC_SUCCESSES,
    RAFT_METRIC_ENTRIES_SENT,
    RAFT_METRIC_COUNT
} raft_metric_id_t;

#define RAFT_METRICS_WIRE_BYTES (8u + 8u * (size_t)RAFT_METRIC_COUNT)

typedef struct {
    atomic_uint_fast64_t value[RAFT_METRIC_COUNT];
    uint64_t started_ms;
} raft_metrics_t;

typedef struct {
    uint64_t value[RAFT_METRIC_COUNT];
} raft_metrics_sample_t;

void raft_metrics_init(raft_metrics_t *metrics, uint64_t started_ms);
void raft_metrics_increment(raft_metrics_t *metrics, raft_metric_id_t id);
void raft_metrics_add(raft_metrics_t *metrics, raft_metric_id_t id, uint64_t count);
void raft_metrics_connection_open(raft_metrics_t *metrics);
void raft_metrics_connection_close(raft_metrics_t *metrics);
void raft_metrics_capture(const raft_metrics_t *metrics, raft_metrics_sample_t *sample);
const char *raft_metrics_name(raft_metric_id_t id);
/* Encoders validate fixed schema and do not modify output on rejection. */
int raft_metrics_encode(const raft_metrics_sample_t *sample, uint8_t *out, size_t capacity);
int raft_metrics_decode(const uint8_t *bytes, size_t length, raft_metrics_sample_t *out);

#endif /* RAFT_METRICS_H */
