#include "raft_append.h"
#include "raft_wire.h"
#include <string.h>

static void put_u16(uint8_t *p, uint16_t n)
{ p[0] = (uint8_t)(n >> 8); p[1] = (uint8_t)n; }
static uint16_t get_u16(const uint8_t *p)
{ return (uint16_t)((unsigned)p[0] << 8 | p[1]); }
static void put_u32(uint8_t *p, uint32_t n)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (24u - 8u*i)); }
static void put_u64(uint8_t *p, uint64_t n)
{ for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(n >> (56u - 8u*i)); }
static uint32_t get_u32(const uint8_t *p)
{ uint32_t n = 0; for (unsigned i = 0; i < 4; ++i) n = (n << 8) | p[i]; return n; }
static uint64_t get_u64(const uint8_t *p)
{ uint64_t n = 0; for (unsigned i = 0; i < 8; ++i) n = (n << 8) | p[i]; return n; }

static int valid_request(const raft_append_request_t *r)
{
    return r != NULL && r->term != 0 && r->leader_id != RAFT_NO_VOTE &&
           r->prev_log_term <= r->term &&
           ((r->prev_log_index == 0 && r->prev_log_term == 0) ||
            (r->prev_log_index > 0 && r->prev_log_term != 0));
}
raft_status_t raft_append_encode_request(const raft_append_request_t *r,
                                         uint8_t out[RAFT_APPEND_REQUEST_BYTES])
{
    if (!valid_request(r) || out == NULL) return RAFT_ERR_INVALID_ARGUMENT;
    put_u64(out, r->term); put_u32(out+8, r->leader_id);
    put_u64(out+12, r->prev_log_index); put_u64(out+20, r->prev_log_term);
    put_u64(out+28, r->leader_commit); return RAFT_OK;
}
raft_status_t raft_append_decode_request(const uint8_t *data, size_t len,
                                         raft_append_request_t *r)
{
    if (data == NULL || r == NULL || len != RAFT_APPEND_REQUEST_BYTES)
        return RAFT_ERR_INVALID_ARGUMENT;
    raft_append_request_t tmp = {.term=get_u64(data), .leader_id=get_u32(data+8),
        .prev_log_index=get_u64(data+12), .prev_log_term=get_u64(data+20),
        .leader_commit=get_u64(data+28)};
    if (!valid_request(&tmp)) return RAFT_ERR_INVALID_ARGUMENT;
    *r = tmp; return RAFT_OK;
}
raft_status_t raft_append_encode_response(const raft_append_response_t *r,
                                          uint8_t out[RAFT_APPEND_RESPONSE_BYTES])
{
    if (r == NULL || out == NULL || (r->success != 0 && r->success != 1))
        return RAFT_ERR_INVALID_ARGUMENT;
    put_u64(out, r->term); out[8] = (uint8_t)r->success; return RAFT_OK;
}
raft_status_t raft_append_decode_response(const uint8_t *data, size_t len,
                                          raft_append_response_t *r)
{
    if (data == NULL || r == NULL || len != RAFT_APPEND_RESPONSE_BYTES || data[8] > 1)
        return RAFT_ERR_INVALID_ARGUMENT;
    *r = (raft_append_response_t){.term=get_u64(data), .success=data[8]};
    return RAFT_OK;
}

static int valid_batch(const raft_append_batch_t *batch)
{
    if (batch == NULL || !valid_request(&batch->header) ||
        batch->entry_count > RAFT_APPEND_MAX_ENTRIES ||
        batch->entry_count > UINT64_MAX - batch->header.prev_log_index)
        return 0;
    raft_term_t previous_term = batch->header.prev_log_term;
    for (size_t i = 0; i < batch->entry_count; ++i) {
        const raft_log_entry_t *entry = &batch->entries[i];
        if (entry->term == 0 || entry->term < previous_term ||
            entry->term > batch->header.term ||
            entry->command_len > UINT16_MAX ||
            (entry->command_len && entry->command == NULL)) return 0;
        previous_term = entry->term;
    }
    return 1;
}

raft_status_t raft_append_encode_batch(const raft_append_batch_t *batch,
                                       uint8_t *out, size_t capacity,
                                       size_t *written)
{
    if (out == NULL || written == NULL || !valid_batch(batch))
        return RAFT_ERR_INVALID_ARGUMENT;
    size_t needed = RAFT_APPEND_REQUEST_BYTES;
    if (batch->entry_count) {
        needed += 2;
        for (size_t i = 0; i < batch->entry_count; ++i) {
            size_t extra = 10 + batch->entries[i].command_len;
            if (extra > RAFT_WIRE_MAX_PAYLOAD - needed)
                return RAFT_ERR_INVALID_ARGUMENT;
            needed += extra;
        }
    }
    if (needed > capacity || needed > RAFT_WIRE_MAX_PAYLOAD)
        return RAFT_ERR_INVALID_ARGUMENT;
    if (raft_append_encode_request(&batch->header, out) != RAFT_OK)
        return RAFT_ERR_INVALID_ARGUMENT;
    size_t position = RAFT_APPEND_REQUEST_BYTES;
    if (batch->entry_count) {
        put_u16(out + position, (uint16_t)batch->entry_count); position += 2;
        for (size_t i = 0; i < batch->entry_count; ++i) {
            const raft_log_entry_t *entry = &batch->entries[i];
            put_u64(out + position, entry->term); position += 8;
            put_u16(out + position, (uint16_t)entry->command_len); position += 2;
            if (entry->command_len) {
                memcpy(out + position, entry->command, entry->command_len);
                position += entry->command_len;
            }
        }
    }
    *written = position;
    return RAFT_OK;
}

raft_status_t raft_append_decode_batch(const uint8_t *data, size_t length,
                                       raft_append_batch_t *out)
{
    if (data == NULL || out == NULL || length < RAFT_APPEND_REQUEST_BYTES ||
        length > RAFT_WIRE_MAX_PAYLOAD)
        return RAFT_ERR_INVALID_ARGUMENT;
    raft_append_batch_t tmp = {0};
    if (raft_append_decode_request(data, RAFT_APPEND_REQUEST_BYTES, &tmp.header)
        != RAFT_OK) return RAFT_ERR_INVALID_ARGUMENT;
    if (length == RAFT_APPEND_REQUEST_BYTES) {
        *out = tmp;
        return RAFT_OK;
    }
    if (length < RAFT_APPEND_REQUEST_BYTES + 2) return RAFT_ERR_INVALID_ARGUMENT;
    size_t position = RAFT_APPEND_REQUEST_BYTES;
    size_t count = get_u16(data + position); position += 2;
    if (count == 0 || count > RAFT_APPEND_MAX_ENTRIES)
        return RAFT_ERR_INVALID_ARGUMENT;
    tmp.entry_count = count;
    for (size_t i = 0; i < count; ++i) {
        if (length - position < 10) return RAFT_ERR_INVALID_ARGUMENT;
        raft_log_entry_t *entry = &tmp.entries[i];
        entry->term = get_u64(data + position); position += 8;
        entry->command_len = get_u16(data + position); position += 2;
        if (entry->command_len > length - position)
            return RAFT_ERR_INVALID_ARGUMENT;
        entry->command = entry->command_len ? (uint8_t *)(data + position) : NULL;
        position += entry->command_len;
    }
    if (position != length || !valid_batch(&tmp))
        return RAFT_ERR_INVALID_ARGUMENT;
    *out = tmp;
    return RAFT_OK;
}

raft_status_t raft_append_handle_batch(raft_node_t *node,
                                       const raft_append_batch_t *batch,
                                       uint64_t now_ms,
                                       raft_append_response_t *reply)
{
    if (node == NULL || reply == NULL || !valid_batch(batch) ||
        !raft_node_is_valid(node) || !node->election_timer.armed ||
        batch->header.leader_id == node->id)
        return RAFT_ERR_INVALID_ARGUMENT;
    const raft_append_request_t *r = &batch->header;
    if (r->term < node->current_term) {
        *reply = (raft_append_response_t){node->current_term, 0};
        return RAFT_OK;
    }
    raft_election_timer_t timer = node->election_timer;
    raft_status_t result = raft_election_timer_reset(&timer, now_ms);
    if (result != RAFT_OK) return result;
    if (r->term > node->current_term) {
        node->current_term = r->term;
        node->voted_for = RAFT_NO_VOTE;
    }
    node->role = RAFT_FOLLOWER;
    node->election_timer = timer;

    raft_index_t matched = 0;
    result = raft_log_reconcile(&node->log, r->prev_log_index,
                                 r->prev_log_term, batch->entries,
                                 batch->entry_count, node->commit_index,
                                 &matched);
    if (result == RAFT_ERR_LOG_MISMATCH ||
        result == RAFT_ERR_COMMITTED_CONFLICT) {
        *reply = (raft_append_response_t){node->current_term, 0};
        return RAFT_OK;
    }
    if (result != RAFT_OK) return result;
    if (r->leader_commit > node->commit_index) {
        raft_index_t target = r->leader_commit < matched ? r->leader_commit : matched;
        if (target > node->commit_index) node->commit_index = target;
    }
    *reply = (raft_append_response_t){node->current_term, 1};
    return RAFT_OK;
}

raft_status_t raft_append_handle(raft_node_t *node,
                                 const raft_append_request_t *r,
                                 uint64_t now_ms,
                                 raft_append_response_t *reply)
{
    if (r == NULL) return RAFT_ERR_INVALID_ARGUMENT;
    raft_append_batch_t batch = {.header = *r, .entry_count = 0};
    return raft_append_handle_batch(node, &batch, now_ms, reply);
}

raft_status_t raft_append_handle_response(raft_node_t *node,
                                          const raft_append_response_t *r,
                                          uint64_t now_ms)
{
    if (node == NULL || r == NULL || (r->success != 0 && r->success != 1) ||
        !raft_node_is_valid(node) || !node->election_timer.armed)
        return RAFT_ERR_INVALID_ARGUMENT;
    if (r->term <= node->current_term) return RAFT_OK;
    raft_election_timer_t timer = node->election_timer;
    raft_status_t result = raft_election_timer_reset(&timer, now_ms);
    if (result != RAFT_OK) return result;
    node->role = RAFT_FOLLOWER;
    node->voted_for = RAFT_NO_VOTE;
    node->current_term = r->term;
    node->election_timer = timer;
    return RAFT_OK;
}
