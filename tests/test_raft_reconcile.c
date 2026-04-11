#include "raft_log.h"
#include "raft_node.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)

static void append(raft_log_t *log, raft_term_t term, const char *payload)
{
    CHECK(raft_log_append(log, term, payload, strlen(payload)) == RAFT_OK);
}
static void verify(const raft_log_t *log, raft_index_t index,
                   raft_term_t term, const char *payload)
{
    const raft_log_entry_t *entry = raft_log_at(log, index);
    CHECK(entry != NULL && entry->term == term);
    CHECK(entry->command_len == strlen(payload));
    CHECK(memcmp(entry->command, payload, strlen(payload)) == 0);
}
#define ENTRY(t, s) (raft_log_entry_t){(t), (uint8_t *)(s), sizeof(s)-1}

static void test_empty_append_and_duplicate(void)
{
    raft_log_t log;
    raft_log_init(&log);
    raft_log_entry_t incoming[] = {ENTRY(1,"abc"), ENTRY(1,"def")};
    raft_index_t match = 999;
    CHECK(raft_log_matches(&log, 0, 0));
    CHECK(!raft_log_matches(&log, 0, 1));
    CHECK(raft_log_reconcile(&log, 0, 0, incoming, 2, 0, &match) == RAFT_OK);
    CHECK(match == 2 && log.count == 2);
    verify(&log, 1, 1, "abc");
    verify(&log, 2, 1, "def");
    incoming[0].command = (uint8_t *)"xyz";
    verify(&log, 1, 1, "abc"); /* deep copy survives source changes */
    raft_log_entry_t duplicate[] = {ENTRY(1,"abc"), ENTRY(1,"def")};
    const uint8_t *original_pointer = raft_log_at(&log, 1)->command;
    CHECK(raft_log_reconcile(&log, 0, 0, duplicate, 2, 2, &match) == RAFT_OK);
    CHECK(raft_log_at(&log, 1)->command == original_pointer); /* idempotence */
    CHECK(match == 2 && log.count == 2);
    raft_log_destroy(&log);
}

static void test_mismatched_prev_and_term_lookup(void)
{
    raft_log_t log;
    raft_log_init(&log);
    append(&log, 1, "a");
    append(&log, 2, "b");
    raft_term_t last = 99;
    CHECK(raft_log_last_term(&log, &last) == RAFT_OK && last == 2);
    CHECK(raft_log_matches(&log, 2, 2));
    CHECK(!raft_log_matches(&log, 2, 3));
    CHECK(!raft_log_matches(&log, 3, 2));
    CHECK(!raft_log_matches(NULL, 0, 0));
    raft_index_t match = 777;
    raft_log_entry_t entry[] = {ENTRY(3, "c")};
    CHECK(raft_log_reconcile(&log, 2, 1, entry, 1, 0, &match) == RAFT_ERR_LOG_MISMATCH);
    CHECK(raft_log_reconcile(&log, 3, 2, entry, 1, 0, &match) == RAFT_ERR_LOG_MISMATCH);
    CHECK(match == 777 && log.count == 2);
    verify(&log, 2, 2, "b");
    raft_log_destroy(&log);
}

static void test_matching_prefix_does_not_delete_tail(void)
{
    raft_log_t log;
    raft_log_init(&log);
    append(&log, 1, "a");
    append(&log, 2, "b");
    append(&log, 3, "c");
    raft_index_t match = 999;
    raft_log_entry_t same[] = {ENTRY(2,"b")};
    CHECK(raft_log_reconcile(&log, 1, 1, same, 1, 2, &match) == RAFT_OK);
    CHECK(match == 2 && log.count == 3);
    CHECK(raft_log_reconcile(&log, 1, 1, NULL, 0, 2, &match) == RAFT_OK);
    CHECK(match == 1 && log.count == 3);
    verify(&log, 3, 3, "c");
    raft_log_destroy(&log);
}

static void test_conflict_rewrites_uncommitted_suffix(void)
{
    raft_log_t log;
    raft_log_init(&log);
    append(&log, 1, "a");
    append(&log, 2, "old-two");
    append(&log, 3, "old-three");
    raft_log_entry_t incoming[] = {ENTRY(4,"new-two"), ENTRY(4,"new-three")};
    raft_index_t match = 0;
    CHECK(raft_log_reconcile(&log, 1, 1, incoming, 2, 1, &match) == RAFT_OK);
    CHECK(log.count == 3 && match == 3);
    verify(&log, 1, 1, "a");
    verify(&log, 2, 4, "new-two");
    verify(&log, 3, 4, "new-three");
    CHECK(raft_log_reconcile(&log, 3, 4, NULL, 0, 1, &match) == RAFT_OK);
    CHECK(log.count == 3);
    raft_log_destroy(&log);
}

static void test_overlap_then_extension(void)
{
    raft_log_t log;
    raft_log_init(&log);
    append(&log, 1, "a");
    append(&log, 2, "b");
    raft_log_entry_t incoming[] = {ENTRY(2,"b"), ENTRY(3,"c"), ENTRY(3,"d")};
    raft_index_t match = 0;
    CHECK(raft_log_reconcile(&log, 1, 1, incoming, 3, 2, &match) == RAFT_OK);
    CHECK(log.count == 4 && match == 4);
    verify(&log, 2, 2, "b");
    verify(&log, 3, 3, "c");
    verify(&log, 4, 3, "d");
    raft_log_destroy(&log);
}

static void test_committed_conflict_rejected_atomically(void)
{
    raft_node_t node;
    CHECK(raft_node_init(&node, 1) == RAFT_OK);
    node.current_term = 6;
    append(&node.log, 1, "a");
    append(&node.log, 2, "b");
    append(&node.log, 3, "c");
    node.commit_index = 2;
    node.last_applied = 1;
    CHECK(raft_node_is_valid(&node));
    raft_log_entry_t conflict[] = {ENTRY(4,"modified")};
    raft_index_t matched = 987;
    CHECK(raft_log_reconcile(&node.log, 1, 1, conflict, 1, node.commit_index,
                             &matched) == RAFT_ERR_COMMITTED_CONFLICT);
    CHECK(matched == 987 && node.log.count == 3);
    verify(&node.log, 2, 2, "b");
    CHECK(raft_node_is_valid(&node));
    raft_log_entry_t safe[] = {ENTRY(4,"replacement")};
    CHECK(raft_log_reconcile(&node.log, 2, 2, safe, 1, node.commit_index,
                             &matched) == RAFT_OK);
    verify(&node.log, 3, 4, "replacement");
    CHECK(raft_node_is_valid(&node));
    raft_node_destroy(&node);
}

static void test_validation_and_binary_data(void)
{
    raft_log_t log;
    raft_log_init(&log);
    append(&log, 2, "start");
    raft_index_t match = 4321;
    raft_log_entry_t regresses[] = {ENTRY(3,"higher"), ENTRY(2,"lower")};
    CHECK(raft_log_reconcile(&log, 1, 2, regresses, 2, 0, &match) == RAFT_ERR_INVALID_ARGUMENT);
    raft_log_entry_t missing = {3, NULL, 2};
    CHECK(raft_log_reconcile(&log, 1, 2, &missing, 1, 0, &match) == RAFT_ERR_INVALID_ARGUMENT);
    raft_log_entry_t zero = {0, NULL, 0};
    CHECK(raft_log_reconcile(&log, 1, 2, &zero, 1, 0, &match) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_log_reconcile(&log, 0, 2, NULL, 0, 0, &match) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_log_reconcile(&log, 1, 0, NULL, 0, 0, &match) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_log_reconcile(&log, 1, 2, NULL, 1, 0, &match) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_log_reconcile(&log, 1, 2, NULL, 0, 2, &match) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_log_reconcile(&log, 1, 2, &missing, SIZE_MAX, 0, &match) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_log_reconcile(NULL, 0, 0, NULL, 0, 0, &match) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_log_reconcile(&log, 0, 0, NULL, 0, 0, NULL) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_log_last_term(NULL, &(raft_term_t){0}) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_log_last_term(&log, NULL) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(log.count == 1 && match == 4321);
    uint8_t bytes[] = {0, 0xff, 0x41, 0};
    raft_log_entry_t binary[] = {{2, bytes, sizeof(bytes)}};
    CHECK(raft_log_reconcile(&log, 1, 2, binary, 1, 1, &match) == RAFT_OK);
    bytes[1] = 0;
    const raft_log_entry_t *entry = raft_log_at(&log, 2);
    CHECK(entry && entry->command_len == 4 && entry->command[1] == 0xff);
    CHECK(match == 2);
    raft_log_destroy(&log);
}

int main(void)
{
    test_empty_append_and_duplicate();
    test_mismatched_prev_and_term_lookup();
    test_matching_prefix_does_not_delete_tail();
    test_conflict_rewrites_uncommitted_suffix();
    test_overlap_then_extension();
    test_committed_conflict_rejected_atomically();
    test_validation_and_binary_data();
    printf("PASS: %u Phase 11 log reconciliation assertions\n", checks);
    return 0;
}
