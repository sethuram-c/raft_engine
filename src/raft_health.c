#include "raft_health.h"

#include <string.h>

static void store32(uint8_t *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (24u - i * 8u));
}

static void store64(uint8_t *p, uint64_t n)
{
    for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(n >> (56u - i * 8u));
}

static uint32_t load32(const uint8_t *p)
{
    uint32_t result = 0;
    for (unsigned i = 0; i < 4; ++i) result = (result << 8) | p[i];
    return result;
}

static uint64_t load64(const uint8_t *p)
{
    uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i) result = (result << 8) | p[i];
    return result;
}

/* A forged or malformed READY flag is rejected rather than trusted. */
static int valid(const raft_health_sample_t *s)
{
    if (!s || !s->node_id || s->role > 2 ||
        (s->flags & ~RAFT_HEALTH_FLAGS_MASK) != 0 ||
        s->last_applied > s->commit_index) return 0;
    int recent = (s->flags & RAFT_HEALTH_QUORUM_RECENT) != 0;
    int ready = (s->flags & RAFT_HEALTH_WRITE_READY) != 0;
    if (recent && (s->role != 2 || s->quorum_age_ms > RAFT_HEALTH_FRESH_MS)) return 0;
    if (!recent && s->quorum_age_ms != UINT64_MAX) return 0;
    if (ready && (!recent || s->role != 2 ||
        (s->flags & (RAFT_HEALTH_ELECTION_ENABLED | RAFT_HEALTH_STORAGE_OK)) !=
        (RAFT_HEALTH_ELECTION_ENABLED | RAFT_HEALTH_STORAGE_OK))) return 0;
    return 1;
}

int raft_health_evaluate(const raft_health_input_t *input, raft_health_sample_t *out)
{
    if (!input || !out || input->node_id == 0 || input->role > 2 ||
        input->last_applied > input->commit_index ||
        input->now_ms < input->started_ms) return -1;
    raft_health_sample_t tmp = {0};
    tmp.node_id = input->node_id;
    tmp.role = input->role;
    tmp.term = input->term;
    tmp.commit_index = input->commit_index;
    tmp.last_applied = input->last_applied;
    tmp.uptime_ms = input->now_ms - input->started_ms;
    tmp.quorum_age_ms = UINT64_MAX;
    if (input->election_enabled) tmp.flags |= RAFT_HEALTH_ELECTION_ENABLED;
    if (input->storage_healthy) tmp.flags |= RAFT_HEALTH_STORAGE_OK;
    if (input->role == 2 && input->ack_valid &&
        input->last_ack_term == input->term &&
        input->now_ms >= input->last_ack_ms &&
        input->now_ms - input->last_ack_ms <= RAFT_HEALTH_FRESH_MS) {
        tmp.quorum_age_ms = input->now_ms - input->last_ack_ms;
        tmp.flags |= RAFT_HEALTH_QUORUM_RECENT;
        if (input->election_enabled && input->storage_healthy)
            tmp.flags |= RAFT_HEALTH_WRITE_READY;
    }
    if (!valid(&tmp)) return -1;
    *out = tmp;
    return 0;
}

int raft_health_write_ready(const raft_health_sample_t *sample)
{
    return valid(sample) && (sample->flags & RAFT_HEALTH_WRITE_READY) != 0;
}

int raft_health_encode(const raft_health_sample_t *sample, uint8_t *bytes, size_t size)
{
    if (!valid(sample) || !bytes || size < RAFT_HEALTH_WIRE_BYTES) return -1;
    uint8_t result[RAFT_HEALTH_WIRE_BYTES] = {'R', 'H', 'L', 'T', 1, 0, 0, 0};
    result[5] = sample->role;
    result[6] = sample->flags;
    store32(result + 8, sample->node_id);
    store64(result + 12, sample->term);
    store64(result + 20, sample->commit_index);
    store64(result + 28, sample->last_applied);
    store64(result + 36, sample->quorum_age_ms);
    store64(result + 44, sample->uptime_ms);
    memcpy(bytes, result, sizeof(result));
    return 0;
}

int raft_health_decode(const uint8_t *bytes, size_t size, raft_health_sample_t *out)
{
    if (!bytes || !out || size != RAFT_HEALTH_WIRE_BYTES ||
        memcmp(bytes, "RHLT", 4) != 0 || bytes[4] != 1 || bytes[7] != 0) return -1;
    raft_health_sample_t tmp = {0};
    tmp.node_id = load32(bytes + 8);
    tmp.role = bytes[5];
    tmp.flags = bytes[6];
    tmp.term = load64(bytes + 12);
    tmp.commit_index = load64(bytes + 20);
    tmp.last_applied = load64(bytes + 28);
    tmp.quorum_age_ms = load64(bytes + 36);
    tmp.uptime_ms = load64(bytes + 44);
    if (!valid(&tmp)) return -1;
    *out = tmp;
    return 0;
}
