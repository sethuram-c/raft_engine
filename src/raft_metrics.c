#include "raft_metrics.h"

#include <string.h>

static const char *const names[RAFT_METRIC_COUNT] = {
    "node_id", "uptime_ms", "current_term", "role", "last_log_index",
    "commit_index", "last_applied", "snapshot_index", "election_rounds_total",
    "leader_transitions_total", "vote_requests_total", "votes_granted_total",
    "append_requests_total", "append_rejections_total",
    "replication_rpc_failures_total", "snapshots_installed_total",
    "client_commits_total", "client_timeouts_total",
    "tls_handshake_failures_total", "tls_reload_successes_total",
    "tls_reload_failures_total", "connections_accepted_total",
    "connections_active", "connections_peak", "rejected_frames_total",
    "max_follower_lag", "storage_failures_total", "replication_rpc_successes_total",
    "entries_sent_total"
};

const char *raft_metrics_name(raft_metric_id_t id)
{
    return id >= 0 && id < RAFT_METRIC_COUNT ? names[id] : NULL;
}

void raft_metrics_init(raft_metrics_t *metrics, uint64_t started_ms)
{
    if (!metrics) return;
    for (size_t i = 0; i < RAFT_METRIC_COUNT; ++i)
        atomic_init(&metrics->value[i], 0);
    metrics->started_ms = started_ms;
}

void raft_metrics_add(raft_metrics_t *metrics, raft_metric_id_t id, uint64_t count)
{
    if (!metrics || id < 0 || id >= RAFT_METRIC_COUNT) return;
    (void)atomic_fetch_add_explicit(&metrics->value[id], count, memory_order_relaxed);
}

void raft_metrics_increment(raft_metrics_t *metrics, raft_metric_id_t id)
{
    raft_metrics_add(metrics, id, 1);
}

void raft_metrics_connection_open(raft_metrics_t *metrics)
{
    if (!metrics) return;
    uint_fast64_t active = atomic_fetch_add_explicit(
        &metrics->value[RAFT_METRIC_CONNECTIONS_ACTIVE], 1,
        memory_order_relaxed) + 1;
    atomic_uint_fast64_t *peak = &metrics->value[RAFT_METRIC_CONNECTIONS_PEAK];
    uint_fast64_t existing = atomic_load_explicit(peak, memory_order_relaxed);
    while (existing < active &&
           !atomic_compare_exchange_weak_explicit(peak, &existing, active,
                memory_order_relaxed, memory_order_relaxed)) { }
}

void raft_metrics_connection_close(raft_metrics_t *metrics)
{
    if (!metrics) return;
    (void)atomic_fetch_sub_explicit(
        &metrics->value[RAFT_METRIC_CONNECTIONS_ACTIVE], 1,
        memory_order_relaxed);
}

void raft_metrics_capture(const raft_metrics_t *metrics, raft_metrics_sample_t *sample)
{
    if (!metrics || !sample) return;
    for (size_t i = 0; i < RAFT_METRIC_COUNT; ++i)
        sample->value[i] = (uint64_t)atomic_load_explicit(&metrics->value[i],
                                                         memory_order_relaxed);
    /* open() raises active before updating peak; a concurrent scrape can
     * sample between those two operations. Never expose peak < active. */
    if (sample->value[RAFT_METRIC_CONNECTIONS_PEAK] <
        sample->value[RAFT_METRIC_CONNECTIONS_ACTIVE])
        sample->value[RAFT_METRIC_CONNECTIONS_PEAK] =
            sample->value[RAFT_METRIC_CONNECTIONS_ACTIVE];
}

static void store64(uint8_t *p, uint64_t n)
{
    for (unsigned i = 0; i < 8; ++i)
        p[i] = (uint8_t)(n >> (56u - 8u * i));
}

static uint64_t load64(const uint8_t *p)
{
    uint64_t n = 0;
    for (unsigned i = 0; i < 8; ++i) n = (n << 8) | p[i];
    return n;
}

int raft_metrics_encode(const raft_metrics_sample_t *sample, uint8_t *out, size_t capacity)
{
    if (!sample || !out || capacity < RAFT_METRICS_WIRE_BYTES ||
        sample->value[RAFT_METRIC_ROLE] > 2 ||
        sample->value[RAFT_METRIC_NODE_ID] == 0 ||
        sample->value[RAFT_METRIC_LAST_APPLIED] > sample->value[RAFT_METRIC_COMMIT_INDEX] ||
        sample->value[RAFT_METRIC_COMMIT_INDEX] > sample->value[RAFT_METRIC_LAST_LOG_INDEX] ||
        sample->value[RAFT_METRIC_CONNECTIONS_ACTIVE] >
            sample->value[RAFT_METRIC_CONNECTIONS_PEAK]) return -1;
    const uint8_t header[8] = {'R', 'M', 'E', 'T', 0, 1, 0, RAFT_METRIC_COUNT};
    memcpy(out, header, sizeof(header));
    for (size_t i = 0; i < RAFT_METRIC_COUNT; ++i)
        store64(out + 8 + i * 8, sample->value[i]);
    return 0;
}

int raft_metrics_decode(const uint8_t *bytes, size_t length, raft_metrics_sample_t *out)
{
    const uint8_t header[8] = {'R', 'M', 'E', 'T', 0, 1, 0, RAFT_METRIC_COUNT};
    if (!bytes || !out || length != RAFT_METRICS_WIRE_BYTES ||
        memcmp(bytes, header, sizeof(header)) != 0) return -1;
    raft_metrics_sample_t tmp;
    for (size_t i = 0; i < RAFT_METRIC_COUNT; ++i)
        tmp.value[i] = load64(bytes + 8 + i * 8);
    if (!tmp.value[RAFT_METRIC_NODE_ID] || tmp.value[RAFT_METRIC_ROLE] > 2 ||
        tmp.value[RAFT_METRIC_LAST_APPLIED] > tmp.value[RAFT_METRIC_COMMIT_INDEX] ||
        tmp.value[RAFT_METRIC_COMMIT_INDEX] > tmp.value[RAFT_METRIC_LAST_LOG_INDEX] ||
        tmp.value[RAFT_METRIC_CONNECTIONS_ACTIVE] >
            tmp.value[RAFT_METRIC_CONNECTIONS_PEAK]) return -1;
    *out = tmp;
    return 0;
}
