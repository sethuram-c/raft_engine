#ifndef RAFT_LOG_H
#define RAFT_LOG_H

#include "raft_types.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
    raft_term_t term;
    uint8_t *command;
    size_t command_len;
} raft_log_entry_t;

typedef struct {
    raft_log_entry_t *entries;
    size_t count;
    size_t capacity;
    raft_index_t base_index; /* last compacted index (virtual retained boundary) */
    raft_term_t base_term;   /* term at base_index; 0/0 before snapshots */
} raft_log_t;

void raft_log_init(raft_log_t *log);
void raft_log_destroy(raft_log_t *log);
raft_status_t raft_log_append(raft_log_t *log, raft_term_t term,
                              const void *command, size_t command_len);
raft_index_t raft_log_last_index(const raft_log_t *log);
const raft_log_entry_t *raft_log_at(const raft_log_t *log, raft_index_t index);
raft_status_t raft_log_term_at(const raft_log_t *log, raft_index_t index,
                                raft_term_t *out_term);
raft_status_t raft_log_truncate_from(raft_log_t *log, raft_index_t first_index);
/* Drop through index, retaining its index/term as an implicit boundary.
 * Caller must first ensure index <= lastApplied <= commitIndex. */
raft_status_t raft_log_compact_prefix(raft_log_t *log, raft_index_t index,
                                      raft_term_t term);

/* A compacted log has a virtual (base_index,base_term) boundary.
 * Before compaction this is logical index 0 with term 0. */
int raft_log_matches(const raft_log_t *log, raft_index_t index,
                     raft_term_t term);
raft_status_t raft_log_last_term(const raft_log_t *log, raft_term_t *out_term);

/* Reconcile a follower's in-memory log with a single AppendEntries batch.
 * Incoming entries are indexed prev_index+1 ... prev_index+entry_count.
 * - Reject a missing/mismatched (prev_index, prev_term).
 * - Preserve matching prefix entries (index+term), even with duplicates.
 * - Replace the first conflicting suffix; never truncate on an empty or
 *   wholly matching batch, even when local log has extra entries.
 * - Reject modification of entries at/below committed_index.
 * - Validate/copy everything BEFORE mutating log (failure atomicity).
 * - On success, out_last_matched is prev_index + entry_count.
 *
 * Caller owns incoming command buffers; success copies them as needed.
 * Caller MUST serialize access to log and committed_index externally.
 * This is an in-memory preparation API: no TCP or persistence in Phase 11.
 */
raft_status_t raft_log_reconcile(raft_log_t *log,
                                 raft_index_t prev_index,
                                 raft_term_t prev_term,
                                 const raft_log_entry_t *entries,
                                 size_t entry_count,
                                 raft_index_t committed_index,
                                 raft_index_t *out_last_matched);

#endif
