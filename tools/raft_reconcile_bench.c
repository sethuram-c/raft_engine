#define _POSIX_C_SOURCE 200809L
/* Phase 33: isolate follower AppendEntries reconciliation from CLI/TCP overhead.
 * This tool is intentionally compatible with the Phase 32 raft_log.c baseline. */
#include "raft_log.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t clock_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) { perror("clock_gettime"); exit(2); }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

int main(int argc, char **argv)
{
    char *end = NULL;
    unsigned long n = argc >= 2 ? strtoul(argv[1], &end, 10) : 8192;
    if ((argc >= 2 && (*end != '\0' || n == 0)) || n > 100000) {
        fputs("usage: raft-reconcile-bench [entries: 1..100000]\n", stderr);
        return 2;
    }
    raft_log_t log;
    raft_log_init(&log);
    char command[32] = "SET benchmark value";
    raft_log_entry_t incoming = {1, (uint8_t *)command, strlen(command)};
    uint64_t begin = clock_ns();
    for (unsigned long i = 0; i < n; ++i) {
        raft_index_t matched = 0;
        if (raft_log_reconcile(&log, i, i ? 1 : 0,
                               &incoming, 1, 0, &matched) != RAFT_OK ||
            matched != i + 1) {
            fprintf(stderr, "FAIL at %lu\n", i);
            raft_log_destroy(&log);
            return 1;
        }
    }
    uint64_t elapsed = clock_ns() - begin;
    if (raft_log_last_index(&log) != n ||
        raft_log_at(&log, n)->command_len != strlen(command) ||
        memcmp(raft_log_at(&log, n)->command, command, strlen(command)) != 0) {
        fputs("FAIL: data mismatch\n", stderr);
        raft_log_destroy(&log);
        return 1;
    }
    printf("entries=%lu elapsed_ms=%.3f capacity=%zu\n", n,
           (double)elapsed / 1000000.0, log.capacity);
    raft_log_destroy(&log);
    return 0;
}
