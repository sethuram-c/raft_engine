#ifndef RAFT_TCP_H
#define RAFT_TCP_H

#include "raft_config.h"

#include <stddef.h>

/*
 * Bounded thread-per-connection TCP transport for PING and Raft voting.
 */
#define RAFT_TCP_BACKLOG 16
#define RAFT_TCP_CONNECT_TIMEOUT_MS 1000

/* Opens a listening TCP socket bound to the exact peer IPv4 address and port.
 * Returns a descriptor >= 0 on success (caller owns and must close it),
 * or -1 on failure and writes a diagnostic into error when available.
 */
int raft_tcp_listen(const raft_peer_config_t *local,
                    char *error, size_t error_capacity);

/* Connects to the specified peer using a bounded, nonblocking connect.
 * Returns a connected descriptor >= 0 on success (caller owns it), or -1.
 * Phase 4 callers close the descriptor without sending any bytes.
 */
int raft_tcp_connect(const raft_peer_config_t *peer, int timeout_ms,
                     char *error, size_t error_capacity);

/* Accepts clients into a bounded worker pool (8 concurrent requests).
 * When all workers are busy, the accept loop waits for a free slot; the
 * kernel listen backlog holds queued connections.
 * max_connections=0 serves until SIGINT or SIGTERM; otherwise drains and
 * exits after N accepted connections. Signals interrupt active sockets.
 * Returns 0 on clean termination and -1 on fatal server errors.
 * Phase 9 automatic voting is opt-in via raft_tcp_elect().
 */
int raft_tcp_serve(const raft_cluster_config_t *config,
                   unsigned int max_connections,
                   char *error, size_t error_capacity);

/* Run real, automatic RequestVote elections on three nodes. No heartbeats,
 * durable terms/votes, or log replication yet. */
int raft_tcp_elect(const raft_cluster_config_t *config,
                  char *error, size_t error_capacity);

#endif /* RAFT_TCP_H */
