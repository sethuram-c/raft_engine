#ifndef RAFT_LOG_H
#define RAFT_LOG_H

#include "raft_types.h"

#include <stddef.h>
#include <stdint.h>

/* An entry owns its command buffer. Its index is its 1-based position. */
typedef struct {
    raft_term_t term;
    uint8_t *command;
    size_t command_len;
} raft_log_entry_t;

typedef struct {
    raft_log_entry_t *entries;
    size_t count;
    size_t capacity;
} raft_log_t;

/* Initialize exactly once before use. Destroy when finished. */
void raft_log_init(raft_log_t *log);
void raft_log_destroy(raft_log_t *log);

/* Copy the command into owned memory. Zero-length commands are allowed. */
raft_status_t raft_log_append(raft_log_t *log, raft_term_t term,
                              const void *command, size_t command_len);

/* Index 0 is reserved; the first real entry is at index 1. */
raft_index_t raft_log_last_index(const raft_log_t *log);
const raft_log_entry_t *raft_log_at(const raft_log_t *log,
                                    raft_index_t index);

/* The virtual entry at index 0 has term 0. Other missing indices fail. */
raft_status_t raft_log_term_at(const raft_log_t *log, raft_index_t index,
                                raft_term_t *out_term);

/* Discard indices >= first_index; first_index == last_index+1 is a no-op. */
raft_status_t raft_log_truncate_from(raft_log_t *log,
                                     raft_index_t first_index);

#endif /* RAFT_LOG_H */
