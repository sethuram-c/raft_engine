#include "raft_replication.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #e); exit(1); } } while (0)

static void ready_leader(raft_node_t *node)
{
    CHECK(raft_node_init(node, 1) == RAFT_OK);
    CHECK(raft_node_start_follower(node, 100, 42) == RAFT_OK);
    node->role = RAFT_LEADER;
    node->current_term = 3;
}

int main(void)
{
    raft_node_t node;
    ready_leader(&node);
    raft_replication_progress_t p;
    CHECK(raft_replication_progress_init(&p, 2, 3, 0) == RAFT_OK);
    CHECK(p.next_index == 1 && p.match_index == 0);
    raft_append_batch_t batch;
    raft_index_t sent = UINT64_MAX;
    CHECK(raft_replication_prepare(&node, &p, &batch, &sent) == RAFT_OK);
    CHECK(batch.entry_count == 0 && sent == 0 && batch.header.prev_log_index == 0);
    raft_append_response_t ok = {3, 1}, reject = {3, 0}, stale = {2, 1}, newer = {4, 0};
    CHECK(raft_replication_observe(&p, 0, &ok) == RAFT_OK);
    CHECK(p.next_index == 1 && p.match_index == 0);
    const uint8_t binary[] = {'A', 0, 'B'};
    CHECK(raft_log_append(&node.log, 3, binary, sizeof(binary)) == RAFT_OK);
    CHECK(raft_log_append(&node.log, 3, "SET b 2", 7) == RAFT_OK);
    CHECK(raft_replication_prepare(&node, &p, &batch, &sent) == RAFT_OK);
    CHECK(batch.entry_count == 2 && sent == 2 && batch.header.prev_log_index == 0);
    CHECK(batch.entries[0].command_len == 3 && batch.entries[0].command[1] == 0);
    CHECK(raft_replication_observe(&p, 2, &ok) == RAFT_OK);
    CHECK(p.next_index == 3 && p.match_index == 2);
    CHECK(raft_replication_prepare(&node, &p, &batch, &sent) == RAFT_OK);
    CHECK(batch.entry_count == 0 && batch.header.prev_log_index == 2 && sent == 2);
    CHECK(raft_replication_observe(&p, 2, &reject) == RAFT_OK);
    CHECK(p.next_index == 2 && p.match_index == 1);
    CHECK(raft_replication_prepare(&node, &p, &batch, &sent) == RAFT_OK);
    CHECK(batch.header.prev_log_index == 1 && batch.entry_count == 1 && sent == 2);
    CHECK(raft_replication_observe(&p, 2, &reject) == RAFT_OK);
    CHECK(p.next_index == 1 && p.match_index == 0);
    CHECK(raft_replication_observe(&p, 2, &reject) == RAFT_OK);
    CHECK(p.next_index == 1);
    CHECK(raft_replication_observe(&p, 2, &stale) == RAFT_OK);
    CHECK(p.next_index == 1 && p.match_index == 0);
    CHECK(raft_replication_observe(&p, 2, &newer) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_replication_observe(&p, 2, &ok) == RAFT_OK);
    CHECK(p.next_index == 3 && p.match_index == 2);
    CHECK(raft_replication_progress_init(&p, 2, 4, 2) == RAFT_OK);
    CHECK(p.next_index == 3 && p.match_index == 0);
    CHECK(raft_replication_prepare(&node, &p, &batch, &sent) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_replication_progress_init(NULL, 2, 3, 1) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_replication_progress_init(&p, 0, 3, 1) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_replication_progress_init(&p, 2, 0, 1) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_replication_progress_init(&p, 2, 3, UINT64_MAX) == RAFT_ERR_INVALID_ARGUMENT);
    /* 16-entry / 4096-byte batching; exact nextIndex holds beyond first batch. */
    raft_log_destroy(&node.log);
    raft_log_init(&node.log);
    for (int i = 0; i < 18; ++i) CHECK(raft_log_append(&node.log, 3, "z", 1) == RAFT_OK);
    CHECK(raft_replication_progress_init(&p, 2, 3, 18) == RAFT_OK);
    CHECK(raft_replication_observe(&p, 18, &reject) == RAFT_OK);
    CHECK(p.next_index == 18 && p.match_index == 0);
    CHECK(raft_replication_progress_init(&p, 2, 3, 0) == RAFT_OK);
    CHECK(raft_replication_prepare(&node, &p, &batch, &sent) == RAFT_OK);
    CHECK(batch.entry_count == 16 && sent == 16);
    CHECK(raft_replication_observe(&p, sent, &ok) == RAFT_OK);
    CHECK(raft_replication_prepare(&node, &p, &batch, &sent) == RAFT_OK);
    CHECK(batch.entry_count == 2 && batch.header.prev_log_index == 16 && sent == 18);
    uint8_t data[RAFT_WIRE_MAX_PAYLOAD]; size_t used = 0;
    CHECK(raft_append_encode_batch(&batch, data, sizeof(data), &used) == RAFT_OK);
    raft_append_batch_t decoded;
    CHECK(raft_append_decode_batch(data, used, &decoded) == RAFT_OK);
    CHECK(decoded.entry_count == 2);
    raft_log_destroy(&node.log); raft_log_init(&node.log);
    char command[1024]; memset(command, 'q', sizeof(command));
    for (int i = 0; i < 7; ++i) CHECK(raft_log_append(&node.log, 3, command, sizeof(command)) == RAFT_OK);
    CHECK(raft_replication_progress_init(&p, 2, 3, 0) == RAFT_OK);
    CHECK(raft_replication_prepare(&node, &p, &batch, &sent) == RAFT_OK);
    CHECK(batch.entry_count == 3 && sent == 3); /* 3*1034+38 <=4096 */
    raft_log_destroy(&node.log); raft_log_init(&node.log);
    char too_big[4096]; memset(too_big, 1, sizeof(too_big));
    CHECK(raft_log_append(&node.log, 3, too_big, sizeof(too_big)) == RAFT_OK);
    CHECK(raft_replication_prepare(&node, &p, &batch, &sent) == RAFT_ERR_INVALID_ARGUMENT);
    raft_node_destroy(&node);
    puts("PASS Phase 13: progress, retry, term, restart, batching, binary, limits");
    return 0;
}
