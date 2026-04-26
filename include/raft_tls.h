#ifndef RAFT_TLS_H
#define RAFT_TLS_H

#include "raft_types.h"
#include "raft_config.h"
#include <stddef.h>
#include <sys/types.h>

/* Phase 26 opt-in TLS 1.3 transport. Context is configured once before
 * launching worker threads, and must not be reconfigured while active. */
int raft_tls_configure(const char *certificate, const char *private_key,
                       const char *ca_bundle, const raft_cluster_config_t *cluster,
                       char *error, size_t capacity);
int raft_tls_enabled(void);
int raft_tls_validate_local_node(raft_node_id_t node_id);
int raft_tls_attach_outbound(int fd, raft_node_id_t expected_node,
                             char *error, size_t capacity);
int raft_tls_attach_inbound(int fd, char *error, size_t capacity);
void raft_tls_detach(void);
raft_node_id_t raft_tls_remote_node(void); /* 0 for authenticated client */
int raft_tls_remote_client(void);
/* These are used only by raft_wire's framing loops, never by disk I/O. */
ssize_t raft_tls_read(int fd, void *buf, size_t size);
ssize_t raft_tls_write(int fd, const void *buf, size_t size);

#endif
