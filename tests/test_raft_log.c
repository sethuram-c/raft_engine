#include "raft_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static void test_empty_log_and_virtual_index(void)
{
    raft_log_t log;
    raft_log_init(&log);
    CHECK(raft_log_last_index(&log) == 0);
    CHECK(raft_log_at(&log, 0) == NULL);
    CHECK(raft_log_at(&log, 1) == NULL);

    raft_term_t term = 99;
    CHECK(raft_log_term_at(&log, 0, &term) == RAFT_OK);
    CHECK(term == 0);
    CHECK(raft_log_term_at(&log, 1, &term) == RAFT_ERR_OUT_OF_RANGE);
    CHECK(term == 0); /* Errors do not overwrite output values. */
    raft_log_destroy(&log);
}

static void test_binary_copy_and_ownership(void)
{
    raft_log_t log;
    raft_log_init(&log);
    unsigned char bytes[] = {'A', '\0', 'B'};
    CHECK(raft_log_append(&log, 1, bytes, sizeof(bytes)) == RAFT_OK);
    bytes[0] = 'Z'; /* Log must own a separate copy. */
    const raft_log_entry_t *entry = raft_log_at(&log, 1);
    CHECK(entry != NULL);
    CHECK(entry->term == 1);
    CHECK(entry->command_len == 3);
    CHECK(entry->command[0] == 'A');
    CHECK(entry->command[1] == '\0');
    CHECK(entry->command[2] == 'B');
    CHECK(raft_log_last_index(&log) == 1);
    raft_log_destroy(&log);
}

static void test_validation_and_term_order(void)
{
    raft_log_t log;
    raft_log_init(&log);
    CHECK(raft_log_append(NULL, 1, "x", 1) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_log_append(&log, 0, "x", 1) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_log_append(&log, 1, NULL, 1) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_log_append(&log, 1, NULL, 0) == RAFT_OK); /* No-op. */
    CHECK(raft_log_append(&log, 2, "a", 1) == RAFT_OK);
    CHECK(raft_log_append(&log, 2, "b", 1) == RAFT_OK);
    CHECK(raft_log_append(&log, 1, "c", 1) == RAFT_ERR_TERM_REGRESSION);
    CHECK(raft_log_last_index(&log) == 3);
    CHECK(raft_log_at(&log, 3)->term == 2);
    CHECK(raft_log_term_at(&log, 0, NULL) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_log_term_at(NULL, 0, &(raft_term_t){0}) == RAFT_ERR_INVALID_ARGUMENT);
    raft_log_destroy(&log);
}

static void test_resize_and_access(void)
{
    raft_log_t log;
    raft_log_init(&log);
    for (unsigned i = 0; i < 40; ++i) {
        CHECK(raft_log_append(&log, 3, &i, sizeof(i)) == RAFT_OK);
    }
    CHECK(raft_log_last_index(&log) == 40);
    CHECK(log.capacity >= 40);
    CHECK(raft_log_at(&log, 41) == NULL);
    for (raft_index_t i = 1; i <= 40; ++i) {
        unsigned stored = 1000;
        CHECK(raft_log_at(&log, i)->command_len == sizeof(stored));
        memcpy(&stored, raft_log_at(&log, i)->command, sizeof(stored));
        CHECK(stored == i - 1);
    }
    raft_log_destroy(&log);
}

static void test_truncation_and_reappend(void)
{
    raft_log_t log;
    raft_log_init(&log);
    CHECK(raft_log_append(&log, 1, "a", 1) == RAFT_OK);
    CHECK(raft_log_append(&log, 2, "b", 1) == RAFT_OK);
    CHECK(raft_log_append(&log, 2, "c", 1) == RAFT_OK);
    CHECK(raft_log_truncate_from(&log, 0) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_log_truncate_from(&log, 5) == RAFT_ERR_OUT_OF_RANGE);
    CHECK(raft_log_truncate_from(&log, 4) == RAFT_OK); /* No-op. */
    CHECK(raft_log_truncate_from(&log, 2) == RAFT_OK);
    CHECK(raft_log_last_index(&log) == 1);
    CHECK(raft_log_at(&log, 2) == NULL);
    CHECK(raft_log_append(&log, 4, "d", 1) == RAFT_OK);
    CHECK(raft_log_last_index(&log) == 2);
    CHECK(raft_log_at(&log, 2)->term == 4);
    CHECK(raft_log_truncate_from(&log, 1) == RAFT_OK);
    CHECK(raft_log_last_index(&log) == 0);
    raft_log_destroy(&log);
    raft_log_destroy(&log); /* Safe to destroy twice. */
}

int main(void)
{
    test_empty_log_and_virtual_index();
    test_binary_copy_and_ownership();
    test_validation_and_term_order();
    test_resize_and_access();
    test_truncation_and_reappend();
    puts("PASS: 5 Raft log unit tests");
    return 0;
}
