#include "raft_append.h"
#include "raft_wire.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { assert(x); ++checks; } while (0)

static raft_node_t new_follower(void)
{
    raft_node_t node;
    CHECK(raft_node_init(&node, 2) == RAFT_OK);
    CHECK(raft_node_start_follower(&node, 100, 123) == RAFT_OK);
    return node;
}

static raft_append_batch_t batch_with(raft_term_t term, raft_index_t prev,
                                     raft_term_t prev_term, const char *command)
{
    raft_append_batch_t b = {0};
    b.header = (raft_append_request_t){term, 1, prev, prev_term, 0};
    if (command) {
        b.entry_count = 1;
        b.entries[0] = (raft_log_entry_t){term, (uint8_t *)command, strlen(command)};
    }
    return b;
}

static void test_encode_decode_binary(void)
{
    uint8_t command[4] = {'A', 0, 'B', 255};
    raft_append_batch_t batch = batch_with(4, 0, 0, NULL);
    batch.entry_count = 2;
    batch.entries[0] = (raft_log_entry_t){3, command, sizeof(command)};
    batch.entries[1] = (raft_log_entry_t){4, NULL, 0};
    uint8_t data[RAFT_WIRE_MAX_PAYLOAD];
    size_t len = 0;
    CHECK(raft_append_encode_batch(&batch, data, sizeof(data), &len) == RAFT_OK);
    CHECK(len == 36 + 2 + 10 + 4 + 10);
    CHECK(data[36] == 0 && data[37] == 2);
    raft_append_batch_t decoded = {0};
    CHECK(raft_append_decode_batch(data, len, &decoded) == RAFT_OK);
    CHECK(decoded.entry_count == 2 && decoded.entries[0].term == 3);
    CHECK(decoded.entries[0].command_len == 4 &&
          memcmp(decoded.entries[0].command, command, 4) == 0);
    CHECK(decoded.entries[1].term == 4 && decoded.entries[1].command_len == 0);
    CHECK(raft_append_decode_batch(data, len - 1, &decoded) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_append_decode_batch(data, len + 1, &decoded) == RAFT_ERR_INVALID_ARGUMENT);
    data[37] = 17;
    CHECK(raft_append_decode_batch(data, len, &decoded) == RAFT_ERR_INVALID_ARGUMENT);
    data[37] = 2;
    data[46] = 255; data[47] = 255; /* first command length too large */
    CHECK(raft_append_decode_batch(data, len, &decoded) == RAFT_ERR_INVALID_ARGUMENT);
    raft_append_batch_t heartbeat = batch_with(4, 0, 0, NULL);
    CHECK(raft_append_encode_batch(&heartbeat, data, sizeof(data), &len) == RAFT_OK);
    CHECK(len == RAFT_APPEND_REQUEST_BYTES);
    CHECK(raft_append_decode_batch(data, len, &decoded) == RAFT_OK &&
          decoded.entry_count == 0);
    CHECK(raft_append_decode_batch(data, 37, &decoded) == RAFT_ERR_INVALID_ARGUMENT);
    batch = batch_with(4, 0, 0, "x");
    batch.entries[0].command_len = RAFT_WIRE_MAX_PAYLOAD;
    CHECK(raft_append_encode_batch(&batch, data, sizeof(data), &len) == RAFT_ERR_INVALID_ARGUMENT);
    batch.entries[0].command_len = 1;
    CHECK(raft_append_encode_batch(&batch, data, 40, &len) == RAFT_ERR_INVALID_ARGUMENT);
}

static void test_append_conflicts_and_commit(void)
{
    raft_node_t n = new_follower();
    raft_append_response_t reply = {0};
    raft_append_batch_t b = batch_with(3, 0, 0, "set a 1");
    CHECK(raft_append_handle_batch(&n, &b, 200, &reply) == RAFT_OK);
    CHECK(reply.success == 1 && reply.term == 3 && n.log.count == 1);
    CHECK(raft_log_at(&n.log, 1)->command_len == strlen("set a 1"));
    CHECK(raft_append_handle_batch(&n, &b, 201, &reply) == RAFT_OK);
    CHECK(n.log.count == 1);
    b = batch_with(3, 2, 3, "bad");
    CHECK(raft_append_handle_batch(&n, &b, 202, &reply) == RAFT_OK);
    CHECK(reply.success == 0 && n.log.count == 1);
    b = batch_with(3, 1, 3, "set b old");
    CHECK(raft_append_handle_batch(&n, &b, 203, &reply) == RAFT_OK);
    CHECK(reply.success == 1 && n.log.count == 2);
    b = batch_with(4, 1, 3, "set b new");
    CHECK(raft_append_handle_batch(&n, &b, 204, &reply) == RAFT_OK);
    CHECK(reply.success == 1 && n.log.count == 2 &&
          raft_log_at(&n.log, 2)->term == 4);
    CHECK(memcmp(raft_log_at(&n.log, 2)->command, "set b new", 9) == 0);
    /* Leader commit=2 acknowledges matched prefix, not local arbitrary tail. */
    b.header.leader_commit = 2;
    CHECK(raft_append_handle_batch(&n, &b, 205, &reply) == RAFT_OK);
    CHECK(n.commit_index == 2);
    b = batch_with(5, 1, 3, "overwrite committed");
    CHECK(raft_append_handle_batch(&n, &b, 206, &reply) == RAFT_OK);
    CHECK(reply.success == 0 && n.commit_index == 2);
    CHECK(raft_log_at(&n.log, 2)->term == 4);
    b = batch_with(4, 1, 3, "stale");
    CHECK(raft_append_handle_batch(&n, &b, 207, &reply) == RAFT_OK);
    CHECK(reply.success == 0 && n.current_term == 5);
    raft_node_destroy(&n);
}

static void test_term_regression_and_limits(void)
{
    raft_append_batch_t batch = batch_with(2, 0, 0, "x");
    batch.entries[0].term = 3;
    uint8_t bytes[RAFT_WIRE_MAX_PAYLOAD];
    size_t len = 0;
    CHECK(raft_append_encode_batch(&batch, bytes, sizeof(bytes), &len) == RAFT_ERR_INVALID_ARGUMENT);
    batch.entries[0].term = 2;
    batch.header.prev_log_index = UINT64_MAX;
    batch.header.prev_log_term = 1;
    CHECK(raft_append_encode_batch(&batch, bytes, sizeof(bytes), &len) == RAFT_ERR_INVALID_ARGUMENT);
    batch.header.prev_log_index = 0; batch.header.prev_log_term = 0;
    batch.entry_count = RAFT_APPEND_MAX_ENTRIES + 1;
    CHECK(raft_append_encode_batch(&batch, bytes, sizeof(bytes), &len) == RAFT_ERR_INVALID_ARGUMENT);
}

int main(void)
{
    test_encode_decode_binary();
    test_append_conflicts_and_commit();
    test_term_regression_and_limits();
    printf("PASS: %u Phase 12 framing, binary, conflict, term, commit assertions\n", checks);
    return 0;
}
