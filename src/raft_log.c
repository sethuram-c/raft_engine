#include "raft_log.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void raft_log_init(raft_log_t *log)
{
    if (log != NULL) {
        log->entries = NULL;
        log->count = 0;
        log->capacity = 0;
    }
}

void raft_log_destroy(raft_log_t *log)
{
    if (log == NULL) {
        return;
    }

    for (size_t i = 0; i < log->count; ++i) {
        free(log->entries[i].command);
    }
    free(log->entries);
    raft_log_init(log);
}

static raft_status_t reserve_one_more(raft_log_t *log)
{
    if (log->count < log->capacity) {
        return RAFT_OK;
    }

    const size_t limit = SIZE_MAX / sizeof(*log->entries);
    if (log->count >= limit) {
        return RAFT_ERR_NO_MEMORY;
    }

    size_t next = 4;
    if (log->capacity != 0) {
        next = (log->capacity > limit / 2) ? limit : log->capacity * 2;
    }
    if (next > limit) {
        next = limit;
    }
    if (next <= log->count) {
        return RAFT_ERR_NO_MEMORY;
    }

    raft_log_entry_t *resized = realloc(log->entries,
                                        next * sizeof(*log->entries));
    if (resized == NULL) {
        return RAFT_ERR_NO_MEMORY;
    }
    log->entries = resized;
    log->capacity = next;
    return RAFT_OK;
}

raft_status_t raft_log_append(raft_log_t *log, raft_term_t term,
                              const void *command, size_t command_len)
{
    if (log == NULL || term == 0 || (command_len != 0 && command == NULL)) {
        return RAFT_ERR_INVALID_ARGUMENT;
    }
    if (log->count != 0 && term < log->entries[log->count - 1].term) {
        return RAFT_ERR_TERM_REGRESSION;
    }

    /* Copy before growing the array: a failed append adds no entry. */
    uint8_t *copy = NULL;
    if (command_len != 0) {
        copy = malloc(command_len);
        if (copy == NULL) {
            return RAFT_ERR_NO_MEMORY;
        }
        memcpy(copy, command, command_len);
    }

    raft_status_t status = reserve_one_more(log);
    if (status != RAFT_OK) {
        free(copy);
        return status;
    }
    log->entries[log->count].term = term;
    log->entries[log->count].command = copy;
    log->entries[log->count].command_len = command_len;
    ++log->count;
    return RAFT_OK;
}

raft_index_t raft_log_last_index(const raft_log_t *log)
{
    return log == NULL ? 0 : (raft_index_t)log->count;
}

const raft_log_entry_t *raft_log_at(const raft_log_t *log,
                                    raft_index_t index)
{
    if (log == NULL || index == 0 || index > log->count) {
        return NULL;
    }
    return &log->entries[(size_t)(index - 1)];
}

raft_status_t raft_log_term_at(const raft_log_t *log, raft_index_t index,
                                raft_term_t *out_term)
{
    if (log == NULL || out_term == NULL) {
        return RAFT_ERR_INVALID_ARGUMENT;
    }
    if (index == 0) {
        *out_term = 0;
        return RAFT_OK;
    }
    const raft_log_entry_t *entry = raft_log_at(log, index);
    if (entry == NULL) {
        return RAFT_ERR_OUT_OF_RANGE;
    }
    *out_term = entry->term;
    return RAFT_OK;
}

raft_status_t raft_log_truncate_from(raft_log_t *log,
                                     raft_index_t first_index)
{
    if (log == NULL || first_index == 0) {
        return RAFT_ERR_INVALID_ARGUMENT;
    }
    /* Subtraction avoids overflow when the log becomes very large. */
    if (first_index - 1 > log->count) {
        return RAFT_ERR_OUT_OF_RANGE;
    }

    const size_t first = (size_t)(first_index - 1);
    for (size_t i = first; i < log->count; ++i) {
        free(log->entries[i].command);
        log->entries[i].command = NULL;
        log->entries[i].command_len = 0;
    }
    log->count = first;
    return RAFT_OK;
}
