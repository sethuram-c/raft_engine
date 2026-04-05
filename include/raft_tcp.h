#ifndef RAFT_TCP_H
#define RAFT_TCP_H

#include "raft_config.h"

#include <stddef.h>

/*
 * Phase 5: one framed PING per connection, optionally answered with PONG.
 * No Raft RPCs, authentication, or concurrent clients yet.
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

/* Accepts connections sequentially and immediately closes each accepted fd.
 * max_connections=0 serves until SIGINT or SIGTERM; otherwise exits after N.
 * Returns 0 on clean termination and -1 on listener/accept/poll error.
 * This demo intentionally does NOT implement Raft messages yet.
 */
int raft_tcp_serve(const raft_cluster_config_t *config,
                   unsigned int max_connections,
                   char *error, size_t error_capacity);

#endif /* RAFT_TCP_H */
