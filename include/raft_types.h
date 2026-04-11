#ifndef RAFT_TYPES_H
#define RAFT_TYPES_H

#include <stdint.h>

typedef uint64_t raft_term_t;
typedef uint64_t raft_index_t;
typedef uint32_t raft_node_id_t;

#define RAFT_NO_VOTE ((raft_node_id_t)0)

typedef enum {
    RAFT_FOLLOWER = 0,
    RAFT_CANDIDATE,
    RAFT_LEADER
} raft_role_t;

typedef enum {
    RAFT_OK = 0,
    RAFT_ERR_INVALID_ARGUMENT,
    RAFT_ERR_NO_MEMORY,
    RAFT_ERR_OUT_OF_RANGE,
    RAFT_ERR_TERM_REGRESSION,
    RAFT_ERR_LOG_MISMATCH,
    RAFT_ERR_COMMITTED_CONFLICT
} raft_status_t;

#endif
