#include "raft_log.h"
#include <stdlib.h>
#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)
#include <stdio.h>
#include <string.h>

static void test_initial_state(void)
{
    raft_log_t log;
    raft_log_init(&log);
    CHECK(raft_log_last_index(&log) == 0);
    CHECK(raft_log_at(&log, 0) == NULL);
    CHECK(raft_log_at(&log, 1) == NULL);
    raft_term_t term = 99;
    CHECK(raft_log_term_at(&log, 0, &term) == RAFT_OK && term == 0);
    CHECK(raft_log_term_at(&log, 1, &term) == RAFT_ERR_OUT_OF_RANGE);
    CHECK(term == 0); /* Invalid lookups do not corrupt the out parameter. */
    CHECK(raft_log_last_term(&log, &term) == RAFT_OK && term == 0);
    CHECK(raft_log_term_at(NULL, 0, &term) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_log_term_at(&log, 0, NULL) == RAFT_ERR_INVALID_ARGUMENT);
    raft_log_destroy(&log);
    puts("PASS log: initial state");
}

static void test_copy_and_binary_bytes(void)
{
    raft_log_t log;
    raft_log_init(&log);
    unsigned char command[] = {'X', 0, 'Y'};
    CHECK(raft_log_append(&log, 1, command, sizeof(command)) == RAFT_OK);
    command[0] = 'Z';
    const raft_log_entry_t *entry = raft_log_at(&log, 1);
    CHECK(entry != NULL && entry->term == 1);
    CHECK(entry->command_len == 3 && entry->command[0] == 'X');
    CHECK(entry->command[1] == 0 && entry->command[2] == 'Y');
    raft_log_destroy(&log);
    puts("PASS log: deep copy and binary payload");
}

static void test_indexing_and_empty_command(void)
{
    raft_log_t log;
    raft_log_init(&log);
    CHECK(raft_log_append(&log, 1, NULL, 0) == RAFT_OK);
    CHECK(raft_log_at(&log, 1) != NULL);
    CHECK(raft_log_at(&log, 2) == NULL);
    raft_term_t term = 0;
    CHECK(raft_log_term_at(&log, 1, &term) == RAFT_OK && term == 1);
    CHECK(raft_log_term_at(&log, 2, &term) == RAFT_ERR_OUT_OF_RANGE);
    raft_log_destroy(&log);
    puts("PASS log: virtual prefix and indexing");
}

static void test_truncation(void)
{
    raft_log_t log;
    raft_log_init(&log);
    CHECK(raft_log_append(&log, 1, "a", 1) == RAFT_OK);
    CHECK(raft_log_append(&log, 2, "b", 1) == RAFT_OK);
    CHECK(raft_log_append(&log, 3, "c", 1) == RAFT_OK);
    CHECK(raft_log_truncate_from(&log, 3) == RAFT_OK);
    CHECK(log.count == 2);
    CHECK(raft_log_truncate_from(&log, 3) == RAFT_OK);
    CHECK(raft_log_truncate_from(&log, 0) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_log_truncate_from(NULL, 1) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_log_truncate_from(&log, 4) == RAFT_ERR_OUT_OF_RANGE);
    CHECK(raft_log_append(&log, 4, "d", 1) == RAFT_OK);
    CHECK(raft_log_at(&log, 3)->term == 4);
    raft_log_destroy(&log);
    puts("PASS log: truncate and overwrite suffix");
}

static void test_invalid_append(void)
{
    raft_log_t log;
    raft_log_init(&log);
    CHECK(raft_log_append(NULL, 1, "x", 1) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_log_append(&log, 0, "x", 1) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_log_append(&log, 1, NULL, 1) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_log_append(&log, 3, "a", 1) == RAFT_OK);
    CHECK(raft_log_append(&log, 2, "b", 1) == RAFT_ERR_TERM_REGRESSION);
    CHECK(log.count == 1);
    CHECK(raft_log_matches(&log, 1, 3));
    CHECK(!raft_log_matches(&log, 1, 2));
    CHECK(!raft_log_matches(&log, 2, 3));
    CHECK(raft_log_matches(&log, 0, 0));
    CHECK(!raft_log_matches(&log, 0, 1));
    raft_log_destroy(&log);
    raft_log_destroy(&log);
    puts("PASS log: rejected invalid and regressing entries");
}

int main(void)
{
    test_initial_state();
    test_copy_and_binary_bytes();
    test_indexing_and_empty_command();
    test_truncation();
    test_invalid_append();
    puts("Log unit tests passed (5).");
    return 0;
}
