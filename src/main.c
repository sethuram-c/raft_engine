#include "raft_config.h"
#include "raft_node.h"
#include "raft_tcp.h"
#include "raft_tls.h"
#include "raft_metrics.h"
#include "raft_health.h"
#include "raft_wire.h"
#include "raft_vote.h"
#include "raft_replicate_client.h"
#include "raft_client_auto.h"
#include "raft_wal.h"
#include "raft_snapshot.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/time.h>

static int demo(void)
{
    raft_node_t node;
    if (raft_node_init(&node, 1) != RAFT_OK) return 1;
    const char *a = "SET x 1";
    const char *b = "SET y 2";
    node.current_term = 2;
    if (raft_log_append(&node.log, 1, a, strlen(a)) != RAFT_OK ||
        raft_log_append(&node.log, 2, b, strlen(b)) != RAFT_OK) {
        raft_node_destroy(&node);
        return 1;
    }
    printf("Demo: node=%u state=follower term=%llu entries=%zu commit=%llu applied=%llu\n",
           node.id, (unsigned long long)node.current_term, node.log.count,
           (unsigned long long)node.commit_index,
           (unsigned long long)node.last_applied);
    puts("Demo only: no elections, consensus, or log replication yet.");
    raft_node_destroy(&node);
    return 0;
}

/* Phase 11: demonstrate memory-only suffix repair without network traffic. */
static int log_demo(void)
{
    raft_log_t log;
    raft_log_init(&log);
    if (raft_log_append(&log, 1, "SET x 1", 7) != RAFT_OK ||
        raft_log_append(&log, 2, "SET y old", 9) != RAFT_OK ||
        raft_log_append(&log, 2, "SET z old", 9) != RAFT_OK) {
        raft_log_destroy(&log);
        return 1;
    }
    raft_log_entry_t leader_entries[] = {
        {3, (uint8_t *)"SET y 2", 7},
        {3, (uint8_t *)"SET z 3", 7}
    };
    raft_index_t match = 0;
    raft_status_t result = raft_log_reconcile(&log, 1, 1,
                                             leader_entries, 2, 1, &match);
    if (result != RAFT_OK) {
        raft_log_destroy(&log);
        return 1;
    }
    printf("Log repair: matched_through=%llu total_entries=%zu\n",
           (unsigned long long)match, log.count);
    for (raft_index_t i = 1; i <= raft_log_last_index(&log); ++i) {
        const raft_log_entry_t *entry = raft_log_at(&log, i);
        printf("entry[%llu]: term=%llu command=", (unsigned long long)i,
               (unsigned long long)entry->term);
        (void)fwrite(entry->command, 1, entry->command_len, stdout);
        putchar('\n');
    }
    puts("In-memory demonstration only: no TCP log replication or disk writes.");
    raft_log_destroy(&log);
    return 0;
}

static int load_config(const char *path, raft_cluster_config_t *config)
{
    char error[256];
    raft_config_status_t status =
        raft_config_load_file(path, config, error, sizeof(error));
    if (status != RAFT_CONFIG_OK) {
        fprintf(stderr, "Configuration %s: %s\n",
                raft_config_status_name(status), error);
        return -1;
    }
    return 0;
}

static int check_config(const char *path)
{
    raft_cluster_config_t config;
    if (load_config(path, &config) != 0) return 1;
    printf("Config valid: node_id=%u peers=%zu\n", config.node_id, config.peer_count);
    for (size_t i = 0; i < config.peer_count; ++i) {
        const raft_peer_config_t *peer = &config.peers[i];
        printf("peer %u %s:%u%s\n", peer->id, peer->ipv4,
            (unsigned int)peer->port, peer->id == config.node_id ? " (self)" : "");
    }
    puts("Config only: no sockets opened or nodes started.");
    return 0;
}

static int serve(const char *path, unsigned int max_connections)
{
    raft_cluster_config_t config;
    if (load_config(path, &config) != 0) return 1;
    char error[256];
    if (raft_tcp_serve(&config, max_connections, error, sizeof(error)) != 0) {
        fprintf(stderr, "TCP server error: %s\n", error);
        return 1;
    }
    return 0;
}

static int elect_server(const char *path)
{
    raft_cluster_config_t config;
    if (load_config(path, &config) != 0) return 1;
    char error[256];
    if (raft_tcp_elect(&config, error, sizeof(error)) != 0) {
        fprintf(stderr, "Election server failed: %s\n", error);
        return 1;
    }
    return 0;
}

static int elect_snapshot_server(const char *path)
{
    raft_cluster_config_t config;
    if (load_config(path,&config)!=0) return 1;
    char error[256]={0};
    if (raft_tcp_elect_snapshot(&config,error,sizeof(error))!=0) {
        fprintf(stderr,"Snapshot election server failed: %s\n",error);
        return 1;
    }
    return 0;
}

static int elect_durable_server(const char *path, const char *directory)
{
    raft_cluster_config_t config;
    if (load_config(path, &config) != 0) return 1;
    char error[256] = {0};
    if (raft_tcp_elect_durable(&config, directory, error, sizeof(error)) != 0) {
        fprintf(stderr, "Durable election server failed: %s\n", error);
        return 1;
    }
    return 0;
}

static int elect_snapshot_wal_server(const char *path, const char *directory)
{
    raft_cluster_config_t config;
    if (load_config(path, &config)!=0) return 1;
    char error[256]={0};
    if (raft_tcp_elect_snapshot_wal(&config,directory,error,sizeof(error))!=0) {
        fprintf(stderr,"Snapshot WAL server failed: %s\n",error);
        return 1;
    }
    return 0;
}

static int elect_auth_server(const char *path, const char *keyring,
                            const char *directory)
{
    raft_cluster_config_t config;
    if (load_config(path,&config)!=0) return 1;
    char error[256]={0};
    int result=directory ? raft_tcp_elect_auth_wal(&config,directory,keyring,
                     error,sizeof(error)) :
               raft_tcp_elect_auth(&config,keyring,error,sizeof(error));
    if (result!=0) { fprintf(stderr,"Authenticated server failed: %s\n",error);return 1;}
    return 0;
}

static int elect_wal_server(const char *path, const char *directory)
{
    raft_cluster_config_t config;
    if (load_config(path, &config) != 0) return 1;
    char error[256] = {0};
    if (raft_tcp_elect_wal(&config, directory, error, sizeof(error)) != 0) {
        fprintf(stderr, "WAL election server failed: %s\n", error);
        return 1;
    }
    return 0;
}

/* Offline operation: obtaining the per-node lock prevents concurrent
 * compaction while a server is modifying the WAL. */
static int compact_wal_offline(const char *path, const char *directory)
{
    raft_cluster_config_t config;
    if (load_config(path,&config)!=0) return 1;
    raft_wal_t wal;
    char error[256]={0};
    if (raft_wal_open(&wal,directory,config.node_id,error,sizeof(error))!=0) {
        fprintf(stderr,"WAL checkpoint open: %s\n",error);return 1;
    }
    raft_node_t node;
    if (raft_node_init(&node,config.node_id)!=RAFT_OK) {
        raft_wal_close(&wal);return 1;
    }
    int found=0;
    int ok=raft_wal_load(&wal,&node,&found,error,sizeof(error));
    if (ok==0) ok=raft_wal_compact(&wal,error,sizeof(error));
    if (ok==0)
        printf("WAL checkpoint: node=%u records=%llu term=%llu entries=%llu commit=%llu\n",
               config.node_id,(unsigned long long)wal.sequence,
               (unsigned long long)node.current_term,
               (unsigned long long)raft_log_last_index(&node.log),
               (unsigned long long)node.commit_index);
    else fprintf(stderr,"WAL checkpoint failed: %s\n",error);
    raft_node_destroy(&node);
    raft_wal_close(&wal);
    return ok==0 ? 0 : 1;
}

static int status_peer(const char *path, const char *peer_text)
{
    raft_cluster_config_t config;
    if (load_config(path, &config) != 0) return 1;
    if (peer_text == NULL || *peer_text == '\0' || *peer_text == '-' || *peer_text == '+')
        return 1;
    errno = 0;
    char *end = NULL;
    unsigned long id = strtoul(peer_text, &end, 10);
    if (errno == ERANGE || end == peer_text || *end != '\0' || id == 0 ||
        id > UINT32_MAX) return 1;
    const raft_peer_config_t *peer = raft_config_peer_for_id(&config, (raft_node_id_t)id);
    if (peer == NULL) return 1;
    char error[256];
    int fd = raft_tcp_connect(peer, RAFT_TCP_CONNECT_TIMEOUT_MS, error, sizeof(error));
    if (fd < 0) { fprintf(stderr, "Status connection: %s\n", error); return 1; }
    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        (void)close(fd); return 1;
    }
    raft_wire_status_t result = raft_wire_send(fd, RAFT_WIRE_STATUS_REQUEST,
                                    NULL, 0, error, sizeof(error));
    uint8_t data[17];
    raft_wire_type_t type = RAFT_WIRE_PING;
    size_t length = 0;
    if (result == RAFT_WIRE_OK)
        result = raft_wire_recv(fd, &type, data, sizeof(data), &length,
                                error, sizeof(error));
    (void)close(fd);
    if (result != RAFT_WIRE_OK || type != RAFT_WIRE_STATUS_RESPONSE || length != 17 ||
        data[12] > RAFT_LEADER) {
        fprintf(stderr, "Status reply failed: %s\n", result == RAFT_WIRE_OK ? "invalid reply" : error);
        return 1;
    }
    uint32_t response_id = 0, voted = 0;
    uint64_t term = 0;
    for (int i = 0; i < 4; ++i) {
        response_id = (response_id << 8) | data[i];
        voted = (voted << 8) | data[13+i];
    }
    for (int i = 0; i < 8; ++i) term = (term << 8) | data[4+i];
    if (response_id != id) return 1;
    printf("Status: node=%u role=%s term=%llu voted_for=%u\n", response_id,
           raft_role_name((raft_role_t)data[12]), (unsigned long long)term, voted);
    return 0;
}

static int parse_decimal_u64(const char *text, uint64_t *out);

/* Phase 28 read-only, versioned operational snapshot. The mTLS variant
 * configures the same client credentials as --status-tls before connecting. */
static int metrics_peer(const char *path, const char *peer_text)
{
    raft_cluster_config_t config;
    uint64_t id = 0;
    if (load_config(path, &config) != 0 ||
        parse_decimal_u64(peer_text, &id) != 0 ||
        id == 0 || id > UINT32_MAX) return 1;
    const raft_peer_config_t *peer = raft_config_peer_for_id(&config, (raft_node_id_t)id);
    if (!peer) return 1;
    char error[256] = {0};
    int fd = raft_tcp_connect(peer, RAFT_TCP_CONNECT_TIMEOUT_MS, error, sizeof(error));
    if (fd < 0) {
        fprintf(stderr, "Metrics connection: %s\n", error);
        return 1;
    }
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    int ok = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
             setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
    raft_wire_status_t status = RAFT_WIRE_IO;
    raft_wire_type_t type = RAFT_WIRE_PING;
    uint8_t response[RAFT_METRICS_WIRE_BYTES];
    size_t length = 0;
    if (ok) {
        status = raft_wire_send(fd, RAFT_WIRE_METRICS_REQUEST, NULL, 0,
                                error, sizeof(error));
        if (status == RAFT_WIRE_OK)
            status = raft_wire_recv(fd, &type, response, sizeof(response),
                                    &length, error, sizeof(error));
    }
    raft_tls_detach();
    (void)close(fd);
    raft_metrics_sample_t sample;
    if (!ok || status != RAFT_WIRE_OK || type != RAFT_WIRE_METRICS_RESPONSE ||
        raft_metrics_decode(response, length, &sample) != 0 ||
        sample.value[RAFT_METRIC_NODE_ID] != id) {
        fprintf(stderr, "Metrics response invalid or unavailable: %s\n",
                status == RAFT_WIRE_OK ? "bad schema/identity" : error);
        return 1;
    }
    printf("Metrics: node=%llu source=live volatile=1\n", (unsigned long long)id);
    for (int i = 0; i < RAFT_METRIC_COUNT; ++i)
        printf("raft_%s %llu\n", raft_metrics_name((raft_metric_id_t)i),
               (unsigned long long)sample.value[i]);
    return 0;
}


/* Phase 29: one bounded read-only health RPC. Liveness means the server
 * answered; readiness is an operational heuristic, not a Raft lease.
 * TLS callers must configure the existing authenticated context first. */
static int fetch_health(const raft_cluster_config_t *config, raft_node_id_t id,
                        raft_health_sample_t *out)
{
    const raft_peer_config_t *peer = raft_config_peer_for_id(config, id);
    if (!peer || !out) return -1;
    char error[256] = {0};
    int fd = raft_tcp_connect(peer, 650, error, sizeof(error));
    if (fd < 0) return -1;
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 700000};
    raft_wire_status_t status = RAFT_WIRE_IO;
    raft_wire_type_t reply_type = RAFT_WIRE_PING;
    uint8_t reply[RAFT_HEALTH_WIRE_BYTES];
    size_t length = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0) {
        status = raft_wire_send(fd, RAFT_WIRE_HEALTH_REQUEST, NULL, 0,
                                error, sizeof(error));
        if (status == RAFT_WIRE_OK)
            status = raft_wire_recv(fd, &reply_type, reply, sizeof(reply),
                                    &length, error, sizeof(error));
    }
    raft_tls_detach();
    (void)close(fd);
    raft_health_sample_t tmp;
    if (status != RAFT_WIRE_OK || reply_type != RAFT_WIRE_HEALTH_RESPONSE ||
        raft_health_decode(reply, length, &tmp) != 0 || tmp.node_id != id)
        return -1;
    *out = tmp;
    return 0;
}

static int health_peer(const char *path, const char *id_text)
{
    raft_cluster_config_t config;
    uint64_t id = 0;
    if (load_config(path, &config) != 0 ||
        parse_decimal_u64(id_text, &id) != 0 || id == 0 || id > UINT32_MAX ||
        raft_config_peer_for_id(&config, (raft_node_id_t)id) == NULL) return 1;
    raft_health_sample_t sample;
    if (fetch_health(&config, (raft_node_id_t)id, &sample) != 0) {
        fprintf(stderr, "Health: node=%llu live=0 (unreachable or invalid response)\n",
                (unsigned long long)id);
        return 1;
    }
    printf("Health: node=%u live=1 write_ready=%d role=%s term=%llu "
           "elections=%d storage_ok=%d quorum_recent=%d commit=%llu applied=%llu "
           "quorum_age_ms=", sample.node_id, raft_health_write_ready(&sample),
           raft_role_name((raft_role_t)sample.role), (unsigned long long)sample.term,
           !!(sample.flags & RAFT_HEALTH_ELECTION_ENABLED),
           !!(sample.flags & RAFT_HEALTH_STORAGE_OK),
           !!(sample.flags & RAFT_HEALTH_QUORUM_RECENT),
           (unsigned long long)sample.commit_index,
           (unsigned long long)sample.last_applied);
    if (sample.quorum_age_ms == UINT64_MAX) printf("unknown");
    else printf("%llu", (unsigned long long)sample.quorum_age_ms);
    printf(" uptime_ms=%llu\n", (unsigned long long)sample.uptime_ms);
    return 0;
}

static int cluster_ready(const char *path)
{
    raft_cluster_config_t config;
    if (load_config(path, &config) != 0) return 1;
    unsigned reachable = 0, claimed_leaders = 0, ready_leaders = 0;
    raft_node_id_t leader_id = 0;
    uint64_t leader_term = 0;
    uint64_t highest_observed_term = 0;
    for (size_t i = 0; i < config.peer_count; ++i) {
        raft_health_sample_t sample;
        if (fetch_health(&config, config.peers[i].id, &sample) != 0) continue;
        ++reachable;
        if (sample.term > highest_observed_term) highest_observed_term = sample.term;
        if (sample.role == RAFT_LEADER) ++claimed_leaders;
        if (raft_health_write_ready(&sample)) {
            ++ready_leaders;
            leader_id = sample.node_id;
            leader_term = sample.term;
        }
    }
    int ready = ready_leaders == 1 && claimed_leaders == 1 &&
                highest_observed_term <= leader_term;
    if (ready)
        printf("Readiness: ready=1 leader=%u term=%llu reachable=%u/%zu\n",
               leader_id, (unsigned long long)leader_term, reachable, config.peer_count);
    else
        printf("Readiness: ready=0 leader=unknown reachable=%u/%zu "
               "(no fresh leader quorum, transition, or conflicting reports)\n",
               reachable, config.peer_count);
    return ready ? 0 : 2;
}

static int probe(const char *path, const char *peer_id_text)
{
    raft_cluster_config_t config;
    if (load_config(path, &config) != 0) return 1;
    if (peer_id_text == NULL || *peer_id_text == '\0' || *peer_id_text == '-') {
        fputs("Invalid peer ID: expected positive decimal integer\n", stderr);
        return 1;
    }
    errno = 0;
    char *end = NULL;
    unsigned long id = strtoul(peer_id_text, &end, 10);
    if (errno == ERANGE || end == peer_id_text || *end != '\0' ||
        id == 0 || id > UINT32_MAX) {
        fputs("Invalid peer ID: expected 1..4294967295\n", stderr);
        return 1;
    }
    const raft_peer_config_t *peer =
        raft_config_peer_for_id(&config, (raft_node_id_t)id);
    if (peer == NULL) {
        fprintf(stderr, "Peer ID %lu is not in the cluster\n", id);
        return 1;
    }
    char error[256];
    int fd = raft_tcp_connect(peer, RAFT_TCP_CONNECT_TIMEOUT_MS,
                              error, sizeof(error));
    if (fd < 0) {
        fprintf(stderr, "TCP probe failed: %s\n", error);
        return 1;
    }
    /* Keep the file-descriptor lifecycle inside the TCP module in future.
     * The CLI closes it for now after the connection-only probe. */
    (void)close(fd);
    printf("Connected: local_node=%u peer=%u %s:%u\n", config.node_id,
           peer->id, peer->ipv4, (unsigned)peer->port);
    puts("Probe only: no Raft messages sent.");
    return 0;
}

static int ping_peer(const char *path, const char *peer_id_text,
                     const char *message)
{
    raft_cluster_config_t config;
    if (load_config(path, &config) != 0) return 1;
    if (peer_id_text == NULL || *peer_id_text == '\0' || *peer_id_text == '-') {
        fputs("Invalid peer ID\n", stderr);
        return 1;
    }
    errno = 0;
    char *end = NULL;
    unsigned long id = strtoul(peer_id_text, &end, 10);
    if (errno == ERANGE || end == peer_id_text || *end != '\0' ||
        id == 0 || id > UINT32_MAX) {
        fputs("Invalid peer ID\n", stderr);
        return 1;
    }
    const raft_peer_config_t *peer =
        raft_config_peer_for_id(&config, (raft_node_id_t)id);
    if (peer == NULL) {
        fprintf(stderr, "Peer %lu not configured\n", id);
        return 1;
    }
    size_t message_len = strlen(message);
    if (message_len > RAFT_WIRE_MAX_PAYLOAD) {
        fprintf(stderr, "Message exceeds %u-byte limit\n", RAFT_WIRE_MAX_PAYLOAD);
        return 1;
    }
    char error[256];
    int fd = raft_tcp_connect(peer, RAFT_TCP_CONNECT_TIMEOUT_MS,
                              error, sizeof(error));
    if (fd < 0) {
        fprintf(stderr, "TCP ping connect failed: %s\n", error);
        return 1;
    }
    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        fprintf(stderr, "TCP ping timeout setup failed: %s\n", strerror(errno));
        (void)close(fd);
        return 1;
    }
    uint8_t response[RAFT_WIRE_MAX_PAYLOAD];
    raft_wire_type_t type = RAFT_WIRE_PING;
    size_t response_len = 0;
    raft_wire_status_t status = raft_wire_send(fd, RAFT_WIRE_PING,
                                               message, message_len,
                                               error, sizeof(error));
    if (status == RAFT_WIRE_OK)
        status = raft_wire_recv(fd, &type, response, sizeof(response),
                                &response_len, error, sizeof(error));
    (void)close(fd);
    if (status != RAFT_WIRE_OK || type != RAFT_WIRE_PONG ||
        response_len != message_len ||
        memcmp(response, message, message_len) != 0) {
        fprintf(stderr, "TCP ping failed: %s\n",
                status == RAFT_WIRE_OK ? "PONG mismatch" : error);
        return 1;
    }
    printf("PONG: local_node=%u peer=%u bytes=%zu message=", config.node_id,
           peer->id, response_len);
    (void)fwrite(response, 1, response_len, stdout);
    putchar('\n');
    puts("Demo only: no Raft RPC or consensus performed.");
    return 0;
}

/* Phase 8: send exactly one RequestVote RPC to a configured peer. */
static int parse_decimal_u64(const char *text, uint64_t *out)
{
    if (text == NULL || out == NULL || *text == '\0' || *text == '-' || *text == '+')
        return -1;
    for (const char *p = text; *p != '\0'; ++p)
        if (*p < '0' || *p > '9') return -1;
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0') return -1;
    *out = (uint64_t)value;
    return 0;
}

static int request_vote_peer(const char *path, const char *peer_text,
                             const char *term_text, const char *index_text,
                             const char *log_term_text)
{
    raft_cluster_config_t config;
    if (load_config(path, &config) != 0) return 1;
    uint64_t peer_id, term, last_index, last_term;
    if (parse_decimal_u64(peer_text, &peer_id) != 0 ||
        parse_decimal_u64(term_text, &term) != 0 ||
        parse_decimal_u64(index_text, &last_index) != 0 ||
        parse_decimal_u64(log_term_text, &last_term) != 0 ||
        peer_id == 0 || peer_id > UINT32_MAX || peer_id == config.node_id) {
        fputs("Invalid RequestVote arguments\n", stderr);
        return 1;
    }
    const raft_peer_config_t *peer = raft_config_peer_for_id(&config, (raft_node_id_t)peer_id);
    if (peer == NULL) {
        fputs("RequestVote peer not found in configuration\n", stderr);
        return 1;
    }
    raft_vote_request_t req = {.term=term, .candidate_id=config.node_id,
                               .last_log_index=last_index, .last_log_term=last_term};
    uint8_t encoded[RAFT_VOTE_REQUEST_BYTES];
    if (raft_vote_encode_request(&req, encoded) != RAFT_OK) {
        fputs("Invalid RequestVote term or log metadata\n", stderr);
        return 1;
    }
    char error[256];
    int fd = raft_tcp_connect(peer, RAFT_TCP_CONNECT_TIMEOUT_MS, error, sizeof(error));
    if (fd < 0) {
        fprintf(stderr, "RequestVote connect failed: %s\n", error);
        return 1;
    }
    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        perror("RequestVote socket timeout");
        (void)close(fd);
        return 1;
    }
    raft_wire_status_t status = raft_wire_send(fd, RAFT_WIRE_REQUEST_VOTE,
                                  encoded, sizeof(encoded), error, sizeof(error));
    uint8_t response_bytes[RAFT_VOTE_RESPONSE_BYTES];
    raft_wire_type_t response_type;
    size_t response_length = 0;
    if (status == RAFT_WIRE_OK)
        status = raft_wire_recv(fd, &response_type, response_bytes,
                                sizeof(response_bytes), &response_length,
                                error, sizeof(error));
    (void)close(fd);
    raft_vote_response_t response;
    if (status != RAFT_WIRE_OK || response_type != RAFT_WIRE_REQUEST_VOTE_RESPONSE ||
        raft_vote_decode_response(response_bytes, response_length, &response) != RAFT_OK) {
        fprintf(stderr, "RequestVote failed: %s\n",
                status == RAFT_WIRE_OK ? "invalid vote response" : error);
        return 1;
    }
    printf("Vote response: local_node=%u peer=%u term=%llu granted=%d\n",
           config.node_id, peer->id, (unsigned long long)response.term,
           response.vote_granted);
    puts("Phase 8: one-shot RPC only; no automatic elections or durable voting yet.");
    return 0;
}

static int append_demo_peer(const char *path, const char *peer_text,
                            const char *term_text, const char *prev_index_text,
                            const char *prev_term_text, const char *command)
{
    raft_cluster_config_t config;
    if (load_config(path, &config) != 0) return 1;
    uint64_t peer, term, prev_index, prev_term;
    if (parse_decimal_u64(peer_text, &peer) != 0 || peer > UINT32_MAX ||
        parse_decimal_u64(term_text, &term) != 0 ||
        parse_decimal_u64(prev_index_text, &prev_index) != 0 ||
        parse_decimal_u64(prev_term_text, &prev_term) != 0) {
        fputs("Invalid append-demo arguments\n", stderr);
        return 1;
    }
    return raft_replicate_client_send(&config, (raft_node_id_t)peer, term,
                      prev_index, prev_term, (const uint8_t *)command,
                      strlen(command)) == 0 ? 0 : 1;
}

static int log_status_peer(const char *path, const char *peer_text)
{
    raft_cluster_config_t config;
    if (load_config(path, &config) != 0) return 1;
    uint64_t peer;
    if (parse_decimal_u64(peer_text, &peer) != 0 || peer > UINT32_MAX) return 1;
    return raft_replicate_client_log_status(&config, (raft_node_id_t)peer) == 0 ? 0 : 1;
}

/* Logical-time example: no waiting, network traffic, voting, or role change. */
static int timer_demo(const char *path)
{
    raft_cluster_config_t config;
    if (load_config(path, &config) != 0) return 1;
    raft_node_t node;
    if (raft_node_init(&node, config.node_id) != RAFT_OK) return 1;
    uint64_t now_ms;
    if (raft_clock_monotonic_ms(&now_ms) != RAFT_OK ||
        raft_node_start_follower(&node, now_ms,
                                 0x9E3779B9U ^ config.node_id) != RAFT_OK) {
        raft_node_destroy(&node);
        fputs("Could not initialize follower election timer\n", stderr);
        return 1;
    }
    uint64_t deadline = node.election_timer.deadline_ms;
    printf("Follower timer: node=%u role=%s timeout_ms=%u\n",
           node.id, raft_role_name(node.role), node.election_timer.timeout_ms);
    printf("At deadline-1: timeout_pending=%d\n",
           raft_node_follower_timeout_pending(&node, deadline - 1));
    printf("At deadline: timeout_pending=%d\n",
           raft_node_follower_timeout_pending(&node, deadline));
    if (raft_node_reset_follower_timeout(&node, deadline) != RAFT_OK) {
        raft_node_destroy(&node);
        return 1;
    }
    printf("After reset: timeout_pending=%d next_timeout_ms=%u\n",
           raft_node_follower_timeout_pending(&node, deadline),
           node.election_timer.timeout_ms);
    puts("Demo only: no candidate transition, voting, or leader election yet.");
    raft_node_destroy(&node);
    return 0;
}

static int propose_demo_peer(const char *path, const char *peer_text,
                             const char *command)
{
    raft_cluster_config_t config;
    if (load_config(path, &config) != 0) return 1;
    uint64_t peer;
    if (parse_decimal_u64(peer_text, &peer) != 0 || peer == 0 ||
        peer > UINT32_MAX) return 1;
    size_t len = strlen(command);
    if (len == 0 || len > 1024) {
        fputs("Demo proposal command must contain 1..1024 bytes\n", stderr);
        return 1;
    }
    return raft_replicate_client_demo_propose(&config, (raft_node_id_t)peer,
              (const uint8_t *)command, len) == 0 ? 0 : 1;
}

static int client_write_id_peer(const char *path, const char *peer_text,
                                const char *timeout_text,const char *client_text,
                                const char *request_text,const char *command)
{
    raft_cluster_config_t config;
    if (load_config(path,&config)!=0) return 1;
    uint64_t peer=0,timeout=0,client_id=0,request_id=0;
    if (parse_decimal_u64(peer_text,&peer)!=0 || peer==0 || peer>UINT32_MAX ||
        parse_decimal_u64(timeout_text,&timeout)!=0 || timeout<100 || timeout>3000 ||
        parse_decimal_u64(client_text,&client_id)!=0 || !client_id ||
        parse_decimal_u64(request_text,&request_id)!=0 || !request_id) {
        fputs("Write-id expects peer, timeout 100..3000, nonzero client and request IDs\n",stderr);
        return 1;
    }
    return raft_replicate_client_write_id(&config,(raft_node_id_t)peer,
                 (uint32_t)timeout,client_id,request_id,
                 (const uint8_t *)command,strlen(command));
}

/* Phase 24: topology-only config; no peer ID required. */
static int client_write_auto(const char *path,const char *budget_text,
                             const char *client_text,const char *request_text,
                             const char *command)
{
    raft_cluster_config_t config;
    if (load_config(path,&config)!=0) return 1;
    uint64_t budget=0,client=0,request=0;
    if (parse_decimal_u64(budget_text,&budget)!=0 ||
        budget<RAFT_AUTO_MIN_BUDGET_MS || budget>RAFT_AUTO_MAX_BUDGET_MS ||
        parse_decimal_u64(client_text,&client)!=0 || client==0 ||
        parse_decimal_u64(request_text,&request)!=0 || request==0) {
        fputs("Usage: --write-auto CONFIG BUDGET_MS(500..30000) CLIENT_ID REQUEST_ID COMMAND\n",stderr);
        return 1;
    }
    return raft_client_auto_write(&config,(uint32_t)budget,client,request,
                                 (const uint8_t *)command,strlen(command));
}

static int client_write_auth_auto(const char *path, const char *budget_text,
                                  const char *client_text, const char *request_text,
                                  const char *secret_path, const char *command)
{
    raft_cluster_config_t config;
    uint64_t budget=0,client=0,request=0;
    if (load_config(path,&config)!=0 || parse_decimal_u64(budget_text,&budget)!=0 ||
        budget<RAFT_AUTO_MIN_BUDGET_MS || budget>RAFT_AUTO_MAX_BUDGET_MS ||
        parse_decimal_u64(client_text,&client)!=0 || !client ||
        parse_decimal_u64(request_text,&request)!=0 || !request) return 1;
    return raft_client_auto_write_auth(&config,(uint32_t)budget,client,request,
                  secret_path,(const uint8_t *)command,strlen(command));
}

static int client_write_peer(const char *path, const char *peer_text,
                             const char *timeout_text, const char *command)
{
    raft_cluster_config_t config;
    if (load_config(path, &config) != 0) return 1;
    uint64_t peer = 0, timeout = 0;
    if (parse_decimal_u64(peer_text, &peer) != 0 || peer == 0 ||
        peer > UINT32_MAX || parse_decimal_u64(timeout_text, &timeout) != 0 ||
        timeout < 100 || timeout > 3000) {
        fputs("Write expects valid peer ID and timeout 100..3000 ms\n", stderr);
        return 1;
    }
    size_t length = strlen(command);
    return raft_replicate_client_write(&config, (raft_node_id_t)peer,
                    (uint32_t)timeout, (const uint8_t *)command, length);
}

static int apply_status_peer(const char *path, const char *peer_text)
{
    raft_cluster_config_t config;
    if (load_config(path, &config) != 0) return 1;
    uint64_t peer;
    if (parse_decimal_u64(peer_text, &peer) != 0 || peer == 0 ||
        peer > UINT32_MAX) return 1;
    return raft_replicate_client_apply_status(&config, (raft_node_id_t)peer) == 0 ? 0 : 1;
}

static int get_demo_peer(const char *path, const char *peer_text, const char *key)
{
    raft_cluster_config_t config;
    if (load_config(path, &config) != 0) return 1;
    uint64_t peer;
    if (parse_decimal_u64(peer_text, &peer) != 0 || peer == 0 ||
        peer > UINT32_MAX) return 1;
    return raft_replicate_client_demo_get(&config, (raft_node_id_t)peer,
            (const uint8_t *)key, strlen(key)) == 0 ? 0 : 1;
}

/* Phase 19 offline lab: write a bounded snapshot and trim an in-memory log.
 * This does NOT change a live server or its persistent WAL format. */
static int snapshot_demo(const char *directory)
{
    raft_node_t node, fresh;
    raft_kv_store_t kv, restored;
    raft_snapshot_t snapshot, decoded;
    if (raft_node_init(&node,1)!=RAFT_OK) return 1;
    raft_kv_init(&kv);
    raft_kv_init(&restored);
    raft_snapshot_init(&snapshot);
    raft_snapshot_init(&decoded);
    node.current_term=2;
    int ok = raft_log_append(&node.log,1,"SET alpha one",13)==RAFT_OK &&
             raft_log_append(&node.log,1,"SET beta two",12)==RAFT_OK &&
             raft_log_append(&node.log,2,"DEL alpha",9)==RAFT_OK;
    node.commit_index=2;
    raft_index_t applied=0;
    if (ok) ok=raft_apply_committed(&node,&kv,&applied)==RAFT_OK;
    if (ok) ok=raft_snapshot_capture(&node,&kv,&snapshot)==RAFT_OK;
    if (ok) ok=raft_snapshot_write_file(directory,"phase19.snap",&snapshot)==0;
    if (ok) ok=raft_snapshot_read_file(directory,"phase19.snap",&decoded)==0;
    if (ok) ok=raft_snapshot_compact_applied(&node,&snapshot)==RAFT_OK;
    if (ok) ok=raft_node_init(&fresh,2)==RAFT_OK;
    else memset(&fresh,0,sizeof(fresh));
    if (ok) {
        fresh.current_term=2;
        ok=raft_snapshot_restore_fresh(&decoded,&fresh,&restored)==RAFT_OK;
    }
    if (ok) ok=raft_log_append(&fresh.log,2,"DEL alpha",9)==RAFT_OK;
    if (ok) {
        fresh.commit_index=3;
        ok=raft_apply_committed(&fresh,&restored,&applied)==RAFT_OK;
    }
    if (ok) {
        const uint8_t *value=NULL;
        size_t length=0;
        ok=raft_kv_get(&restored,(const uint8_t *)"alpha",5,&value,&length)==0;
        if (ok) ok=raft_kv_get(&restored,(const uint8_t *)"beta",4,&value,&length)==1 &&
                    length==3 && memcmp(value,"two",3)==0;
    }
    if (ok)
        printf("Snapshot demo: boundary=%llu/%llu retained=%zu replay_applied=%llu keys=%zu file=%s/phase19.snap\n",
            (unsigned long long)node.log.base_index,
            (unsigned long long)node.log.base_term,node.log.count,
            (unsigned long long)fresh.last_applied,restored.count,directory);
    else fputs("Snapshot demo failed (directory must already exist and be writable)\n",stderr);
    raft_node_destroy(&node);
    if (fresh.id) raft_node_destroy(&fresh);
    raft_kv_destroy(&kv); raft_kv_destroy(&restored);
    raft_snapshot_destroy(&snapshot); raft_snapshot_destroy(&decoded);
    return ok ? 0 : 1;
}

static void usage(void)
{
    fputs("Usage: raft-engine [--version | --demo | --check-config PATH | "
          "--log-demo | --snapshot-demo DIRECTORY | --serve PATH [--max-connections N] | --elect PATH | --elect-snapshot PATH | --snapshot-compact-demo PATH LEADER_ID | --elect-durable PATH DATA_DIR | --elect-wal PATH DATA_DIR | --elect-snapshot-wal PATH DATA_DIR | --compact-wal PATH DATA_DIR | --status PATH PEER_ID | --probe PATH PEER_ID | "
          "--ping PATH PEER_ID MESSAGE | --timer-demo PATH | "
          "--request-vote PATH PEER_ID TERM LAST_INDEX LAST_TERM | "
          "--append-demo PATH PEER_ID TERM PREV_INDEX PREV_TERM COMMAND | "
          "--log-status PATH PEER_ID | --apply-status PATH PEER_ID | "
          "--get-demo PATH PEER_ID KEY | --propose-demo PATH LEADER_ID COMMAND | --write PATH PEER_ID TIMEOUT_MS COMMAND | --write-id PATH PEER_ID TIMEOUT_MS CLIENT_ID REQUEST_ID COMMAND | --write-auto PATH BUDGET_MS CLIENT_ID REQUEST_ID COMMAND | --elect-auth PATH KEYRING | --elect-auth-wal PATH DATA_DIR KEYRING | --write-auth-auto PATH BUDGET_MS CLIENT_ID REQUEST_ID SECRET_FILE COMMAND | --elect-tls-auth-wal PATH DATA_DIR KEYRING CERT KEY CA | --status-tls PATH PEER_ID CERT KEY CA | --metrics PATH PEER_ID | --metrics-tls PATH PEER_ID CERT KEY CA | --health PATH PEER_ID | --health-tls PATH PEER_ID CERT KEY CA | --ready PATH | --ready-tls PATH CERT KEY CA | --write-tls-auth-auto PATH BUDGET_MS CLIENT_ID REQUEST_ID SECRET_FILE CERT KEY CA COMMAND]\n", stderr);
}

int main(int argc, char **argv)
{
    if (argc == 1) {
        puts("Raft Engine initialized successfully.");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts("raft-engine 0.1.0");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--demo") == 0) return demo();
    if (argc == 2 && strcmp(argv[1], "--log-demo") == 0) return log_demo();
    if (argc == 3 && strcmp(argv[1], "--snapshot-demo") == 0)
        return snapshot_demo(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--timer-demo") == 0)
        return timer_demo(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--check-config") == 0)
        return check_config(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--elect") == 0)
        return elect_server(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--elect-snapshot") == 0)
        return elect_snapshot_server(argv[2]);
    if (argc == 4 && strcmp(argv[1], "--snapshot-compact-demo") == 0) {
        raft_cluster_config_t config;
        uint64_t peer_id=0;
        if (load_config(argv[2],&config)!=0 ||
            parse_decimal_u64(argv[3],&peer_id)!=0 ||
            peer_id==0 || peer_id>UINT32_MAX || peer_id==config.node_id ||
            raft_config_peer_for_id(&config,(raft_node_id_t)peer_id)==NULL) return 1;
        return raft_replicate_client_snapshot_compact(&config,
                         (raft_node_id_t)peer_id)==0 ? 0 : 1;
    }
    if (argc == 4 && strcmp(argv[1], "--elect-durable") == 0)
        return elect_durable_server(argv[2], argv[3]);
    if (argc == 4 && strcmp(argv[1], "--elect-wal") == 0)
        return elect_wal_server(argv[2], argv[3]);
    if (argc == 4 && strcmp(argv[1], "--elect-snapshot-wal") == 0)
        return elect_snapshot_wal_server(argv[2], argv[3]);
    /* Phase 26: opt-in TLS 1.3 with a trusted CA and mutually authenticated
     * node/client leaf certificates. In authenticated-WAL mode the HMAC
     * keyring still controls client ID admission. */
    if (argc == 8 && strcmp(argv[1], "--elect-tls-auth-wal") == 0) {
        raft_cluster_config_t config;
        char error[256];
        if (load_config(argv[2], &config) != 0 ||
            raft_tls_configure(argv[5], argv[6], argv[7], &config,
                               error, sizeof(error)) != 0) {
            fprintf(stderr, "TLS setup failed: %s\n", error);
            return 1;
        }
        if (raft_tls_enable_reload(config.node_id) != 0) {
            fprintf(stderr, "TLS reload setup failed: local node SAN identity invalid\n");
            return 1;
        }
        return elect_auth_server(argv[2], argv[4], argv[3]);
    }
    if (argc == 11 && strcmp(argv[1], "--write-tls-auth-auto") == 0) {
        raft_cluster_config_t config;
        char error[256];
        if (load_config(argv[2], &config) != 0 ||
            raft_tls_configure(argv[7], argv[8], argv[9], &config,
                               error, sizeof(error)) != 0) {
            fprintf(stderr, "TLS setup failed: %s\n", error);
            return 1;
        }
        return client_write_auth_auto(argv[2],argv[3],argv[4],argv[5],argv[6],argv[10]);
    }
    if (argc == 6 && strcmp(argv[1], "--ready-tls") == 0) {
        raft_cluster_config_t config;
        char error[256] = {0};
        if (load_config(argv[2], &config) != 0 ||
            raft_tls_configure(argv[3], argv[4], argv[5], &config,
                               error, sizeof(error)) != 0) {
            fprintf(stderr, "TLS readiness setup failed: %s\n", error);
            return 1;
        }
        return cluster_ready(argv[2]);
    }
    if (argc == 7 && strcmp(argv[1], "--health-tls") == 0) {
        raft_cluster_config_t config;
        char error[256] = {0};
        if (load_config(argv[2], &config) != 0 ||
            raft_tls_configure(argv[4], argv[5], argv[6], &config,
                               error, sizeof(error)) != 0) {
            fprintf(stderr, "TLS health setup failed: %s\n", error);
            return 1;
        }
        return health_peer(argv[2], argv[3]);
    }
    if (argc == 7 && strcmp(argv[1], "--metrics-tls") == 0) {
        raft_cluster_config_t config;
        char error[256] = {0};
        if (load_config(argv[2], &config) != 0 ||
            raft_tls_configure(argv[4], argv[5], argv[6], &config,
                               error, sizeof(error)) != 0) {
            fprintf(stderr, "TLS metrics setup failed: %s\n", error);
            return 1;
        }
        return metrics_peer(argv[2], argv[3]);
    }
    if (argc == 7 && strcmp(argv[1], "--status-tls") == 0) {
        raft_cluster_config_t config;
        char error[256];
        if (load_config(argv[2], &config) != 0 ||
            raft_tls_configure(argv[4], argv[5], argv[6], &config,
                               error, sizeof(error)) != 0) {
            fprintf(stderr, "TLS setup failed: %s\n", error);
            return 1;
        }
        return status_peer(argv[2], argv[3]);
    }
    if (argc == 4 && strcmp(argv[1], "--elect-auth") == 0)
        return elect_auth_server(argv[2],argv[3],NULL);
    if (argc == 5 && strcmp(argv[1], "--elect-auth-wal") == 0)
        return elect_auth_server(argv[2],argv[4],argv[3]);
    if (argc == 4 && strcmp(argv[1], "--compact-wal") == 0)
        return compact_wal_offline(argv[2], argv[3]);
    if (argc == 4 && strcmp(argv[1], "--status") == 0)
        return status_peer(argv[2], argv[3]);
    if (argc == 4 && strcmp(argv[1], "--metrics") == 0)
        return metrics_peer(argv[2], argv[3]);
    if (argc == 4 && strcmp(argv[1], "--health") == 0)
        return health_peer(argv[2], argv[3]);
    if (argc == 3 && strcmp(argv[1], "--ready") == 0)
        return cluster_ready(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--serve") == 0)
        return serve(argv[2], 0);
    if (argc == 5 && strcmp(argv[1], "--serve") == 0 &&
        strcmp(argv[3], "--max-connections") == 0) {
        errno = 0;
        char *end = NULL;
        unsigned long count = strtoul(argv[4], &end, 10);
        if (argv[4][0] == '\0' || argv[4][0] == '-' || errno == ERANGE ||
            end == argv[4] || *end != '\0' || count == 0 || count > UINT_MAX) {
            fputs("Invalid --max-connections: expected positive integer\n", stderr);
            return 1;
        }
        return serve(argv[2], (unsigned int)count);
    }
    if (argc == 4 && strcmp(argv[1], "--probe") == 0)
        return probe(argv[2], argv[3]);
    if (argc == 5 && strcmp(argv[1], "--ping") == 0)
        return ping_peer(argv[2], argv[3], argv[4]);
    if (argc == 7 && strcmp(argv[1], "--request-vote") == 0)
        return request_vote_peer(argv[2], argv[3], argv[4], argv[5], argv[6]);
    if (argc == 8 && strcmp(argv[1], "--append-demo") == 0)
        return append_demo_peer(argv[2], argv[3], argv[4], argv[5], argv[6], argv[7]);
    if (argc == 4 && strcmp(argv[1], "--log-status") == 0)
        return log_status_peer(argv[2], argv[3]);
    if (argc == 4 && strcmp(argv[1], "--apply-status") == 0)
        return apply_status_peer(argv[2], argv[3]);
    if (argc == 5 && strcmp(argv[1], "--get-demo") == 0)
        return get_demo_peer(argv[2], argv[3], argv[4]);
    if (argc == 8 && strcmp(argv[1], "--write-auth-auto") == 0)
        return client_write_auth_auto(argv[2],argv[3],argv[4],argv[5],argv[6],argv[7]);
    if (argc == 7 && strcmp(argv[1], "--write-auto") == 0)
        return client_write_auto(argv[2],argv[3],argv[4],argv[5],argv[6]);
    if (argc == 8 && strcmp(argv[1], "--write-id") == 0)
        return client_write_id_peer(argv[2],argv[3],argv[4],argv[5],argv[6],argv[7]);
    if (argc == 6 && strcmp(argv[1], "--write") == 0)
        return client_write_peer(argv[2], argv[3], argv[4], argv[5]);
    if (argc == 5 && strcmp(argv[1], "--propose-demo") == 0)
        return propose_demo_peer(argv[2], argv[3], argv[4]);
    usage();
    return 1;
}
