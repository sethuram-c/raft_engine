/* Phase 33: guard the append-only O(n) fast path without asserting wall time.
 * Existing reconciliation / conflict tests remain unchanged. */
#include "raft_log.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)

static void test_geometric_growth_and_idempotence(void)
{
    raft_log_t log;
    raft_log_init(&log);
    size_t growth_events = 0, previous_capacity = 0;
    unsigned char incoming_bytes[] = {'A', 0, 'Z', 255};
    raft_log_entry_t entry = {1, incoming_bytes, sizeof(incoming_bytes)};
    raft_index_t matched = UINT64_MAX;
    for (raft_index_t i = 0; i < 4096; ++i) {
        CHECK(raft_log_reconcile(&log, i, i ? 1 : 0, &entry, 1, 0,
                                 &matched) == RAFT_OK);
        CHECK(matched == i + 1 && log.count == i + 1);
        CHECK(log.capacity >= log.count);
        if (log.capacity != previous_capacity) {
            ++growth_events;
            CHECK(log.capacity > previous_capacity);
            previous_capacity = log.capacity;
        }
    }
    CHECK(growth_events <= 12); /* 4, 8, 16, ..., 4096; not every append. */
    CHECK(log.capacity <= 2 * log.count);
    incoming_bytes[0] = 'B';
    CHECK(raft_log_at(&log, 1)->command[0] == 'A');
    CHECK(raft_log_at(&log, 4096)->command[3] == 255);
    const uint8_t *retained = raft_log_at(&log, 1)->command;
    const size_t old_cap = log.capacity;
    raft_log_entry_t dup = {1, (uint8_t *)"x", 1};
    CHECK(raft_log_reconcile(&log, 0, 0, &dup, 1, 0, &matched) == RAFT_OK);
    CHECK(matched == 1 && log.count == 4096);
    CHECK(log.capacity == old_cap && raft_log_at(&log, 1)->command == retained);
    raft_log_destroy(&log);
}

static void test_large_batch_copy_and_boundary(void)
{
    raft_log_t log;
    raft_log_init(&log);
    unsigned char command[] = {0x00, 0x01, 0xff};
    raft_log_entry_t entries[40];
    for (size_t i = 0; i < 40; ++i)
        entries[i] = (raft_log_entry_t){.term = 2, .command = command,
                                        .command_len = sizeof(command)};
    raft_index_t matched = 0;
    CHECK(raft_log_reconcile(&log, 0, 0, entries, 40, 0, &matched) == RAFT_OK);
    CHECK(log.count == 40 && matched == 40 && log.capacity >= 40);
    command[2] = 0;
    CHECK(raft_log_at(&log, 40)->command[2] == 0xff);
    CHECK(raft_log_compact_prefix(&log, 24, 2) == RAFT_OK);
    CHECK(log.base_index == 24 && log.count == 16);
    /* Adding a batch after compaction must use *absolute* Raft indices. */
    CHECK(raft_log_reconcile(&log, 40, 2, entries, 40, 24, &matched) == RAFT_OK);
    CHECK(log.count == 56 && raft_log_last_index(&log) == 80 && matched == 80);
    CHECK(raft_log_at(&log, 25)->command[2] == 0xff);
    raft_log_destroy(&log);
}

static void test_conflicts_and_committed_guard(void)
{
    raft_log_t log;
    raft_log_init(&log);
    raft_log_entry_t one = {1, (uint8_t *)"start", 5};
    raft_index_t matched = 0;
    CHECK(raft_log_reconcile(&log, 0, 0, &one, 1, 0, &matched) == RAFT_OK);
    raft_log_entry_t old = {1, (uint8_t *)"old", 3};
    CHECK(raft_log_reconcile(&log, 1, 1, &old, 1, 0, &matched) == RAFT_OK);
    const uint8_t *prefix = raft_log_at(&log, 1)->command;
    raft_log_entry_t newer = {2, (uint8_t *)"new", 3};
    CHECK(raft_log_reconcile(&log, 1, 1, &newer, 1, 2, &matched) ==
          RAFT_ERR_COMMITTED_CONFLICT);
    CHECK(log.count == 2 && raft_log_at(&log, 2)->term == 1);
    CHECK(raft_log_reconcile(&log, 1, 1, &newer, 1, 1, &matched) == RAFT_OK);
    CHECK(log.count == 2 && matched == 2 && raft_log_at(&log, 2)->term == 2);
    CHECK(raft_log_at(&log, 1)->command == prefix);
    CHECK(memcmp(raft_log_at(&log, 2)->command, "new", 3) == 0);
    raft_log_destroy(&log);
}

static void test_alias_input_at_growth_boundary(void)
{
    raft_log_t log;
    raft_log_init(&log);
    raft_log_entry_t first = {1, (uint8_t *)"contents", 8};
    raft_index_t matched = 0;
    for (raft_index_t i = 0; i < 4; ++i)
        CHECK(raft_log_reconcile(&log, i, i ? 1 : 0, &first, 1, 0, &matched) == RAFT_OK);
    CHECK(log.capacity == 4);
    /* Supply an input descriptor stored *inside* the existing log array;
     * this must be staged before realloc moves the old storage. */
    const raft_log_entry_t *aliased = raft_log_at(&log, 1);
    CHECK(raft_log_reconcile(&log, 4, 1, aliased, 1, 0, &matched) == RAFT_OK);
    CHECK(log.capacity >= 5 && log.count == 5);
    CHECK(memcmp(raft_log_at(&log, 5)->command, "contents", 8) == 0);
    raft_log_destroy(&log);
}

int main(void)
{
    test_geometric_growth_and_idempotence();
    test_large_batch_copy_and_boundary();
    test_conflicts_and_committed_guard();
    test_alias_input_at_growth_boundary();
    printf("PASS: %u Phase 33 append growth, aliases, batches, conflict and compaction checks\n", checks);
    return 0;
}
