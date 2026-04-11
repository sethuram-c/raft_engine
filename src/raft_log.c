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
    if (log == NULL) return;
    for (size_t i = 0; i < log->count; ++i) free(log->entries[i].command);
    free(log->entries);
    raft_log_init(log);
}

static raft_status_t reserve_one_more(raft_log_t *log)
{
    const size_t max_entries = SIZE_MAX / sizeof(*log->entries);
    if (log->count >= max_entries) return RAFT_ERR_NO_MEMORY;
    if (log->count < log->capacity) return RAFT_OK;
    size_t next = log->capacity == 0 ? 4 : log->capacity;
    if (next > max_entries / 2) next = max_entries;
    else if (log->capacity != 0) next *= 2;
    raft_log_entry_t *p = realloc(log->entries, next * sizeof(*p));
    if (p == NULL) return RAFT_ERR_NO_MEMORY;
    log->entries = p;
    log->capacity = next;
    return RAFT_OK;
}

raft_status_t raft_log_append(raft_log_t *log, raft_term_t term,
                              const void *command, size_t command_len)
{
    if (log == NULL || term == 0 || (command_len != 0 && command == NULL))
        return RAFT_ERR_INVALID_ARGUMENT;
    if (log->count && term < log->entries[log->count - 1].term)
        return RAFT_ERR_TERM_REGRESSION;
    uint8_t *copy = NULL;
    if (command_len != 0) {
        copy = malloc(command_len);
        if (copy == NULL) return RAFT_ERR_NO_MEMORY;
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

const raft_log_entry_t *raft_log_at(const raft_log_t *log, raft_index_t index)
{
    if (log == NULL || index == 0 || index > log->count) return NULL;
    return &log->entries[(size_t)(index - 1)];
}

raft_status_t raft_log_term_at(const raft_log_t *log, raft_index_t index,
                                raft_term_t *out_term)
{
    if (log == NULL || out_term == NULL) return RAFT_ERR_INVALID_ARGUMENT;
    if (index == 0) { *out_term = 0; return RAFT_OK; }
    const raft_log_entry_t *entry = raft_log_at(log, index);
    if (entry == NULL) return RAFT_ERR_OUT_OF_RANGE;
    *out_term = entry->term;
    return RAFT_OK;
}

raft_status_t raft_log_truncate_from(raft_log_t *log, raft_index_t first_index)
{
    if (log == NULL || first_index == 0) return RAFT_ERR_INVALID_ARGUMENT;
    if (first_index - 1 > log->count) return RAFT_ERR_OUT_OF_RANGE;
    size_t first = (size_t)(first_index - 1);
    for (size_t i = first; i < log->count; ++i) {
        free(log->entries[i].command);
        log->entries[i].command = NULL;
        log->entries[i].command_len = 0;
    }
    log->count = first;
    return RAFT_OK;
}

int raft_log_matches(const raft_log_t *log, raft_index_t index,
                     raft_term_t term)
{
    raft_term_t actual = 0;
    return raft_log_term_at(log, index, &actual) == RAFT_OK && actual == term;
}

raft_status_t raft_log_last_term(const raft_log_t *log, raft_term_t *out_term)
{
    if (log == NULL || out_term == NULL) return RAFT_ERR_INVALID_ARGUMENT;
    return raft_log_term_at(log, raft_log_last_index(log), out_term);
}

raft_status_t raft_log_reconcile(raft_log_t *log,
                                 raft_index_t prev_index,
                                 raft_term_t prev_term,
                                 const raft_log_entry_t *entries,
                                 size_t entry_count,
                                 raft_index_t committed_index,
                                 raft_index_t *out_last_matched)
{
    if (log == NULL || out_last_matched == NULL ||
        (entry_count && entries == NULL) ||
        committed_index > raft_log_last_index(log))
        return RAFT_ERR_INVALID_ARGUMENT;
    /* Validate the logical 0/0 prefix and avoid overflow on all counts. */
    if ((prev_index == 0) != (prev_term == 0))
        return RAFT_ERR_INVALID_ARGUMENT;
    if (entry_count > UINT64_MAX - prev_index)
        return RAFT_ERR_INVALID_ARGUMENT;
    const size_t max_entries = SIZE_MAX / sizeof(*log->entries);
    if (entry_count > max_entries || prev_index > max_entries ||
        entry_count > max_entries - (size_t)prev_index)
        return RAFT_ERR_INVALID_ARGUMENT;
    if (!raft_log_matches(log, prev_index, prev_term))
        return RAFT_ERR_LOG_MISMATCH;

    /* Validate entire input batch before modifying or allocating anything. */
    raft_term_t previous_term = prev_term;
    for (size_t i = 0; i < entry_count; ++i) {
        if (entries[i].term == 0 || entries[i].term < previous_term ||
            (entries[i].command_len != 0 && entries[i].command == NULL))
            return RAFT_ERR_INVALID_ARGUMENT;
        previous_term = entries[i].term;
    }

    const size_t prev = (size_t)prev_index;
    size_t matching = 0;
    while (matching < entry_count && matching < log->count - prev &&
           log->entries[prev + matching].term == entries[matching].term)
        ++matching;

    /* A heartbeat or entirely matching prefix is a no-op (keep local tail). */
    if (matching == entry_count) {
        *out_last_matched = prev_index + entry_count;
        return RAFT_OK;
    }
    const size_t keep = prev + matching;
    if (keep < committed_index) return RAFT_ERR_COMMITTED_CONFLICT;

    /* Construct a replacement array without disturbing log's old ownership. */
    const size_t final_count = prev + entry_count;
    raft_log_entry_t *replacement = malloc(final_count * sizeof(*replacement));
    if (replacement == NULL) return RAFT_ERR_NO_MEMORY;
    if (keep != 0) memcpy(replacement, log->entries, keep * sizeof(*replacement));
    size_t copied = keep;
    for (size_t i = matching; i < entry_count; ++i) {
        uint8_t *payload = NULL;
        if (entries[i].command_len != 0) {
            payload = malloc(entries[i].command_len);
            if (payload == NULL) goto fail;
            memcpy(payload, entries[i].command, entries[i].command_len);
        }
        replacement[copied++] = (raft_log_entry_t){
            .term = entries[i].term,
            .command = payload,
            .command_len = entries[i].command_len
        };
    }

    /* Commit: dispose of old conflicting suffix only after all copies exist. */
    for (size_t i = keep; i < log->count; ++i) free(log->entries[i].command);
    free(log->entries);
    log->entries = replacement;
    log->count = final_count;
    log->capacity = final_count;
    *out_last_matched = prev_index + entry_count;
    return RAFT_OK;

fail:
    for (size_t i = keep; i < copied; ++i) free(replacement[i].command);
    free(replacement);
    return RAFT_ERR_NO_MEMORY;
}
