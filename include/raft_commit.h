#ifndef RAFT_COMMIT_H
#define RAFT_COMMIT_H

#include "raft_replication.h"

/* Phase 14: fixed three-member cluster, leader + exactly two followers.
 * The caller holds the leader's mutex; this function does no I/O.
 *
 * Computes the highest index stored on a voting majority using the leader's
 * own last log index and same-term, distinct-peer matchIndex observations.
 * The Raft current-term rule permits advancing commitIndex only when the
 * entry at that majority index was created in the leader's current term.
 * Advancing that index also implicitly commits every preceding entry.
 *
 * On success *out_commit is the resulting commitIndex; on an invalid call,
 * neither the node nor *out_commit is modified. It does not apply commands.
 */
raft_status_t raft_commit_advance(raft_node_t *leader,
                                  const raft_replication_progress_t *followers,
                                  size_t follower_count,
                                  raft_index_t *out_commit);

#endif
