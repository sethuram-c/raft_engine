#define _POSIX_C_SOURCE 200809L

#include "raft_tcp.h"
#include "raft_tls.h"
#include "raft_wire.h"
#include "raft_vote.h"
#include "raft_election.h"
#include "raft_append.h"
#include "raft_replication.h"
#include "raft_commit.h"
#include "raft_state_machine.h"
#include "raft_storage.h"
#include "raft_wal.h"
#include "raft_snapwal.h"
#include "raft_install.h"
#include "raft_client_write.h"
#include "raft_dedup.h"
#include "raft_auth.h"
#include "raft_metrics.h"
#include "raft_health.h"
#include <stdlib.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <time.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/time.h>
#include <stdatomic.h>
#include <stdint.h>

static volatile sig_atomic_t stop_requested = 0;
static volatile sig_atomic_t tls_reload_requested = 0;

static void on_reload_signal(int signal_number)
{
    (void)signal_number;
    tls_reload_requested = 1;
}

/* Idle timeouts bound an unresponsive client; not an overall frame deadline. */
static int configure_client_timeouts(int fd)
{
    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0 ? 0 : -1;
}


static void on_stop_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void report_error(char *error, size_t capacity, const char *fmt, ...)
{
    if (error == NULL || capacity == 0) return;
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(error, capacity, fmt, args);
    va_end(args);
}

static int validate_endpoint(const raft_peer_config_t *peer,
                             struct sockaddr_in *address,
                             char *error, size_t capacity)
{
    if (peer == NULL || address == NULL || peer->port == 0 ||
        memchr(peer->ipv4, '\0', sizeof(peer->ipv4)) == NULL) {
        report_error(error, capacity, "invalid TCP endpoint");
        return -1;
    }
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_port = htons(peer->port);
    if (inet_pton(AF_INET, peer->ipv4, &address->sin_addr) != 1) {
        report_error(error, capacity, "invalid IPv4 address for peer %u", peer->id);
        return -1;
    }
    return 0;
}

int raft_tcp_listen(const raft_peer_config_t *local,
                    char *error, size_t error_capacity)
{
    struct sockaddr_in address;
    if (validate_endpoint(local, &address, error, error_capacity) < 0)
        return -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        report_error(error, error_capacity, "socket: %s", strerror(errno));
        return -1;
    }

    int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        report_error(error, error_capacity, "setsockopt(SO_REUSEADDR): %s", strerror(errno));
        (void)close(fd);
        return -1;
    }
    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
        report_error(error, error_capacity, "bind %s:%u: %s",
                     local->ipv4, (unsigned)local->port, strerror(errno));
        (void)close(fd);
        return -1;
    }
    if (listen(fd, RAFT_TCP_BACKLOG) < 0) {
        report_error(error, error_capacity, "listen: %s", strerror(errno));
        (void)close(fd);
        return -1;
    }
    if (error != NULL && error_capacity != 0) error[0] = '\0';
    return fd;
}

int raft_tcp_connect(const raft_peer_config_t *peer, int timeout_ms,
                     char *error, size_t error_capacity)
{
    struct sockaddr_in address;
    if (timeout_ms <= 0 ||
        validate_endpoint(peer, &address, error, error_capacity) < 0) {
        if (timeout_ms <= 0)
            report_error(error, error_capacity, "connect timeout must be positive");
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        report_error(error, error_capacity, "socket: %s", strerror(errno));
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        report_error(error, error_capacity, "fcntl(O_NONBLOCK): %s", strerror(errno));
        (void)close(fd);
        return -1;
    }
    int status = connect(fd, (const struct sockaddr *)&address, sizeof(address));
    if (status < 0 && errno == EINPROGRESS) {
        struct pollfd item = {.fd = fd, .events = POLLOUT, .revents = 0};
        do {
            status = poll(&item, 1, timeout_ms);
        } while (status < 0 && errno == EINTR);

        if (status == 0) {
            report_error(error, error_capacity, "connect to %s:%u timed out",
                         peer->ipv4, (unsigned)peer->port);
            (void)close(fd);
            return -1;
        }
        if (status < 0) {
            report_error(error, error_capacity, "poll(connect): %s", strerror(errno));
            (void)close(fd);
            return -1;
        }
        int socket_error = 0;
        socklen_t length = sizeof(socket_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) < 0) {
            report_error(error, error_capacity, "getsockopt(SO_ERROR): %s", strerror(errno));
            (void)close(fd);
            return -1;
        }
        if (socket_error != 0) {
            report_error(error, error_capacity, "connect to %s:%u: %s",
                         peer->ipv4, (unsigned)peer->port, strerror(socket_error));
            (void)close(fd);
            return -1;
        }
    } else if (status < 0) {
        report_error(error, error_capacity, "connect to %s:%u: %s",
                     peer->ipv4, (unsigned)peer->port, strerror(errno));
        (void)close(fd);
        return -1;
    }

    if (fcntl(fd, F_SETFL, flags) < 0) {
        report_error(error, error_capacity, "restore socket flags: %s", strerror(errno));
        (void)close(fd);
        return -1;
    }
    if (raft_tls_enabled()) {
        struct timeval tv = {.tv_sec = timeout_ms / 1000,
                             .tv_usec = (timeout_ms % 1000) * 1000};
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0 ||
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0 ||
            raft_tls_attach_outbound(fd, peer->id, error, error_capacity) != 0) {
            (void)close(fd);
            return -1;
        }
    }
    if (error != NULL && error_capacity != 0) error[0] = '\0';
    return fd;
}

/*
 * Phase 6: bounded thread-per-connection worker slots.
 * A worker owns its accepted client fd. The main thread owns the listener,
 * the worker table, and joins each finished worker before slot reuse.
 */
#define RAFT_TCP_MAX_WORKERS 8u
#define RAFT_TCP_SLOT_WAIT_MS 100L

typedef struct tcp_server tcp_server_t;

typedef struct {
    pthread_t thread;
    tcp_server_t *server;
    int client_fd;
    bool in_use;
    bool finished;
} tcp_worker_t;

struct tcp_server {
    pthread_mutex_t mutex;
    pthread_cond_t worker_changed;
    tcp_worker_t workers[RAFT_TCP_MAX_WORKERS];
    raft_node_id_t node_id;
    raft_metrics_t metrics; /* Atomic event counters; gauges from node mutex. */
    uint64_t health_last_ack_ms; /* guarded by mutex; successful same-term RPC */
    raft_term_t health_last_ack_term;
    bool health_ack_valid;
    raft_node_t vote_node; /* Protected by server->mutex. */
    raft_kv_store_t kv;   /* Same mutex, only committed entries are applied. */
    raft_cluster_config_t config; /* Fixed set of allowed candidate IDs. */
    pthread_t election_thread;
    bool election_thread_started;
    atomic_bool election_stop;
    raft_replication_progress_t progress[RAFT_ELECTION_CLUSTER_SIZE];
    raft_storage_t storage;
    raft_wal_t wal;
    raft_snapwal_t snapwal;
    bool snapshot_journal;
    bool durable;
    bool journal;
    bool storage_failed; /* Read by accept thread only after joining workers. */
    bool auth_mode;
    raft_auth_keys_t auth_keys;
    bool snapshot_mode; /* in-memory or opt-in coordinated snapshot WAL */
    raft_install_receiver_t incoming;
    uint8_t *snapshot_bytes; /* encoded snapshot, immutable while server->mutex held */
    size_t snapshot_length;
    raft_index_t snapshot_index;
    raft_term_t snapshot_term;
    uint32_t snapshot_offsets[RAFT_ELECTION_CLUSTER_SIZE];
};

/* Caller holds server->mutex. Persistence must complete BEFORE any vote,
 * append or proposal acknowledgement or outbound vote for a new term.
 * A write failure is fail-stop: no unrecorded consensus decision is exposed. */
static int durable_checkpoint(tcp_server_t *server)
{
    if (!server->durable && !server->journal && !server->snapshot_journal) return 0;
    char error[256];
    if ((server->snapshot_journal ?
             raft_snapwal_save(&server->snapwal, &server->vote_node,
                               error, sizeof(error)) :
             (server->journal ? raft_wal_save(&server->wal, &server->vote_node,
                               error, sizeof(error)) :
              raft_storage_save(&server->storage, &server->vote_node,
                               error, sizeof(error)))) == 0) return 0;
    raft_metrics_increment(&server->metrics, RAFT_METRIC_STORAGE_FAILURES);
    fprintf(stderr, "FATAL durable storage failure, halting: %s\n", error);
    server->storage_failed = true;
    atomic_store(&server->election_stop, true);
    stop_requested = 1;
    return -1;
}


/* Caller holds server->mutex. The record in the applied state handles
 * compacted history, and the suffix scan prevents duplicate uncommitted
 * entries, including requests inherited from a previous leader. */
static raft_client_write_status_t dedup_admit(tcp_server_t *server,
                              const raft_dedup_entry_t *request,
                              raft_term_t *term, raft_index_t *index)
{
    raft_node_t *node=&server->vote_node;
    raft_kv_store_t *store=&server->kv;
    for (size_t i=0; i<store->session_count; ++i) {
        const raft_kv_session_t *rec=&store->sessions[i];
        if (rec->client_id!=request->client_id) continue;
        if (request->request_id<rec->request_id) return RAFT_CLIENT_WRITE_REJECTED;
        if (request->request_id==rec->request_id) {
            if (rec->command_len!=request->command_len ||
                memcmp(rec->command,request->command,request->command_len))
                return RAFT_CLIENT_WRITE_REJECTED;
            *term=rec->term; *index=rec->index;
            return RAFT_CLIENT_WRITE_COMMITTED;
        }
        break;
    }
    /* Include committed-but-not-applied entries: application may lag. */
    for (raft_index_t n=node->last_applied+1;
         n<=raft_log_last_index(&node->log); ++n) {
        const raft_log_entry_t *entry=raft_log_at(&node->log,n);
        if (!entry) return RAFT_CLIENT_WRITE_UNKNOWN;
        raft_dedup_entry_t stored;
        if (raft_dedup_parse(entry->command,entry->command_len,&stored)!=1 ||
            stored.client_id!=request->client_id) continue;
        if (stored.request_id!=request->request_id ||
            stored.command_len!=request->command_len ||
            memcmp(stored.command,request->command,request->command_len))
            return RAFT_CLIENT_WRITE_REJECTED;
        *term=entry->term; *index=n;
        return n<=node->commit_index ? RAFT_CLIENT_WRITE_COMMITTED :
                                       RAFT_CLIENT_WRITE_PENDING;
    }
    if (store->session_count==RAFT_KV_MAX_SESSIONS) {
        bool known=false;
        for (size_t i=0;i<store->session_count;++i)
            if (store->sessions[i].client_id==request->client_id) known=true;
        if (!known) return RAFT_CLIENT_WRITE_REJECTED;
    }
    uint8_t command[RAFT_DEDUP_PREFIX+RAFT_CLIENT_WRITE_MAX_COMMAND];
    size_t command_len=0;
    if (raft_dedup_wrap(request->client_id,request->request_id,
                        request->command,request->command_len,
                        command,sizeof(command),&command_len)!=0 ||
        raft_log_append(&node->log,node->current_term,command,command_len)!=RAFT_OK)
        return RAFT_CLIENT_WRITE_REJECTED;
    *term=node->current_term; *index=raft_log_last_index(&node->log);
    return durable_checkpoint(server)==0 ? RAFT_CLIENT_WRITE_PENDING :
                                            RAFT_CLIENT_WRITE_UNKNOWN;
}

static raft_client_write_status_t dedup_poll(tcp_server_t *server,
                                             raft_term_t term,raft_index_t index)
{
    const raft_node_t *node=&server->vote_node;
    if (server->storage_failed || node->role!=RAFT_LEADER)
        return RAFT_CLIENT_WRITE_UNKNOWN;
    if (index<=node->log.base_index)
        return node->commit_index>=index ? RAFT_CLIENT_WRITE_COMMITTED :
                                          RAFT_CLIENT_WRITE_UNKNOWN;
    raft_term_t actual_term=0;
    if (raft_log_term_at(&node->log,index,&actual_term)!=RAFT_OK ||
        actual_term!=term) return RAFT_CLIENT_WRITE_UNKNOWN;
    return node->commit_index>=index ? RAFT_CLIENT_WRITE_COMMITTED :
                                       RAFT_CLIENT_WRITE_PENDING;
}

static void process_client(int fd, tcp_server_t *server)
{
    uint8_t payload[RAFT_WIRE_MAX_PAYLOAD];
    raft_wire_type_t type;
    size_t length = 0;
    char message[256] = {0};
    raft_wire_status_t status = raft_wire_recv(fd, &type, payload,
                                   sizeof(payload), &length, message, sizeof(message));
    if (status != RAFT_WIRE_OK && status != RAFT_WIRE_CLOSED)
        raft_metrics_increment(&server->metrics, RAFT_METRIC_REJECTED_FRAMES);
    if (status == RAFT_WIRE_OK && type == RAFT_WIRE_HEALTH_REQUEST && length == 0 &&
        (!raft_tls_enabled() || raft_tls_remote_client() || raft_tls_remote_node())) {
        uint64_t now_ms = 0;
        if (raft_clock_monotonic_ms(&now_ms) != RAFT_OK) return;
        raft_health_input_t input;
        uint8_t result[RAFT_HEALTH_WIRE_BYTES];
        (void)pthread_mutex_lock(&server->mutex);
        raft_node_t *node = &server->vote_node;
        input = (raft_health_input_t){
            .node_id = node->id,
            .role = (uint8_t)node->role,
            .term = node->current_term,
            .commit_index = node->commit_index,
            .last_applied = node->last_applied,
            .started_ms = server->metrics.started_ms,
            .now_ms = now_ms,
            .last_ack_ms = server->health_last_ack_ms,
            .last_ack_term = server->health_last_ack_term,
            .ack_valid = server->health_ack_valid,
            .election_enabled = server->election_thread_started,
            .storage_healthy = !server->storage_failed && !stop_requested
        };
        (void)pthread_mutex_unlock(&server->mutex);
        raft_health_sample_t sample;
        if (raft_health_evaluate(&input, &sample) == 0 &&
            raft_health_encode(&sample, result, sizeof(result)) == 0)
            (void)raft_wire_send(fd, RAFT_WIRE_HEALTH_RESPONSE, result,
                                 sizeof(result), message, sizeof(message));
    } else if (status == RAFT_WIRE_OK && type == RAFT_WIRE_METRICS_REQUEST && length == 0 &&
        (!raft_tls_enabled() || raft_tls_remote_client() || raft_tls_remote_node())) {
        raft_metrics_sample_t sample;
        uint8_t result[RAFT_METRICS_WIRE_BYTES];
        uint64_t now_ms = 0;
        (void)raft_clock_monotonic_ms(&now_ms);
        (void)pthread_mutex_lock(&server->mutex);
        raft_metrics_capture(&server->metrics, &sample);
        raft_node_t *node = &server->vote_node;
        sample.value[RAFT_METRIC_NODE_ID] = node->id;
        sample.value[RAFT_METRIC_UPTIME_MS] = now_ms >= server->metrics.started_ms ?
            now_ms - server->metrics.started_ms : 0;
        sample.value[RAFT_METRIC_TERM] = node->current_term;
        sample.value[RAFT_METRIC_ROLE] = node->role;
        sample.value[RAFT_METRIC_LAST_LOG_INDEX] = raft_log_last_index(&node->log);
        sample.value[RAFT_METRIC_COMMIT_INDEX] = node->commit_index;
        sample.value[RAFT_METRIC_LAST_APPLIED] = node->last_applied;
        sample.value[RAFT_METRIC_SNAPSHOT_INDEX] = node->log.base_index;
        sample.value[RAFT_METRIC_MAX_FOLLOWER_LAG] = 0;
        if (node->role == RAFT_LEADER) {
            for (size_t i = 0; i < server->config.peer_count; ++i) {
                if (server->config.peers[i].id == server->node_id ||
                    server->progress[i].peer_id != server->config.peers[i].id ||
                    server->progress[i].term != node->current_term) continue;
                raft_index_t confirmed = server->progress[i].match_index;
                raft_index_t last = raft_log_last_index(&node->log);
                uint64_t lag = confirmed < last ? last - confirmed : 0;
                if (lag > sample.value[RAFT_METRIC_MAX_FOLLOWER_LAG])
                    sample.value[RAFT_METRIC_MAX_FOLLOWER_LAG] = lag;
            }
        }
        (void)pthread_mutex_unlock(&server->mutex);
        if (raft_metrics_encode(&sample, result, sizeof(result)) == 0)
            (void)raft_wire_send(fd, RAFT_WIRE_METRICS_RESPONSE, result,
                                 sizeof(result), message, sizeof(message));
    } else if (status == RAFT_WIRE_OK && type == RAFT_WIRE_PING) {
        status = raft_wire_send(fd, RAFT_WIRE_PONG, payload, length,
                                message, sizeof(message));
        if (status == RAFT_WIRE_OK) {
            printf("Replied: node=%u bytes=%zu\n", server->node_id, length);
            (void)fflush(stdout);
        }
    } else if (status == RAFT_WIRE_OK && raft_tls_enabled() &&
               (type == RAFT_WIRE_CLIENT_WRITE_AUTH ||
                type == RAFT_WIRE_CLIENT_WRITE_ID ||
                type == RAFT_WIRE_CLIENT_WRITE) &&
               !raft_tls_remote_client()) {
        fputs("Rejected non-client TLS certificate for write RPC\n", stderr);
        return;
    } else if (status == RAFT_WIRE_OK && type == RAFT_WIRE_REQUEST_VOTE) {
        raft_vote_request_t req;
        raft_vote_response_t rsp;
        uint8_t output[RAFT_VOTE_RESPONSE_BYTES];
        uint64_t now_ms = 0;
        if (raft_vote_decode_request(payload, length, &req) != RAFT_OK ||
            raft_config_peer_for_id(&server->config, req.candidate_id) == NULL ||
            req.candidate_id == server->node_id ||
            (raft_tls_enabled() && raft_tls_remote_node() != req.candidate_id) ||
            raft_clock_monotonic_ms(&now_ms) != RAFT_OK) {
            fputs("Rejected malformed RequestVote payload\n", stderr);
            return;
        }
        raft_metrics_increment(&server->metrics, RAFT_METRIC_VOTE_REQUESTS);
        (void)pthread_mutex_lock(&server->mutex);
        raft_term_t old_term = server->vote_node.current_term;
        raft_node_id_t old_vote = server->vote_node.voted_for;
        raft_status_t result = raft_vote_handle(&server->vote_node, &req, now_ms, &rsp);
        if (result == RAFT_OK &&
            (old_term != server->vote_node.current_term ||
             old_vote != server->vote_node.voted_for) &&
            durable_checkpoint(server) != 0)
            result = RAFT_ERR_INVALID_ARGUMENT;
        (void)pthread_mutex_unlock(&server->mutex);
        if (result == RAFT_OK && rsp.vote_granted)
            raft_metrics_increment(&server->metrics, RAFT_METRIC_VOTES_GRANTED);
        if (result != RAFT_OK ||
            raft_vote_encode_response(&rsp, output) != RAFT_OK) {
            fputs("Could not process RequestVote\n", stderr);
            return;
        }
        status = raft_wire_send(fd, RAFT_WIRE_REQUEST_VOTE_RESPONSE,
                                output, sizeof(output), message, sizeof(message));
        if (status == RAFT_WIRE_OK) {
            printf("RequestVote: node=%u candidate=%u term=%llu granted=%d\n",
                   server->node_id, req.candidate_id,
                   (unsigned long long)rsp.term, rsp.vote_granted);
            (void)fflush(stdout);
        }
    } else if (status == RAFT_WIRE_OK && type == RAFT_WIRE_APPEND_ENTRIES) {
        raft_append_batch_t batch;
        raft_append_response_t rsp;
        uint8_t output[RAFT_APPEND_RESPONSE_BYTES];
        uint64_t now_ms = 0;
        if (raft_append_decode_batch(payload, length, &batch) != RAFT_OK ||
            raft_config_peer_for_id(&server->config, batch.header.leader_id) == NULL ||
            batch.header.leader_id == server->node_id ||
            (raft_tls_enabled() && raft_tls_remote_node() != batch.header.leader_id) ||
            raft_clock_monotonic_ms(&now_ms) != RAFT_OK) {
            fputs("Rejected malformed or nonmember AppendEntries\n", stderr);
            return;
        }
        raft_metrics_increment(&server->metrics, RAFT_METRIC_APPEND_REQUESTS);
        (void)pthread_mutex_lock(&server->mutex);
        raft_term_t old_term = server->vote_node.current_term;
        raft_node_id_t old_vote = server->vote_node.voted_for;
        raft_index_t old_commit = server->vote_node.commit_index;
        raft_status_t result = raft_append_handle_batch(&server->vote_node, &batch, now_ms, &rsp);
        raft_index_t last = raft_log_last_index(&server->vote_node.log);
        if (result == RAFT_OK && rsp.success) {
            raft_index_t applied = 0;
            raft_status_t apply_status = raft_apply_committed(&server->vote_node,
                                                               &server->kv, &applied);
            if (apply_status != RAFT_OK)
                fputs("State-machine application deferred (retry on heartbeat)\n", stderr);
        }
        if (result == RAFT_OK &&
            (batch.entry_count != 0 || old_term != server->vote_node.current_term ||
             old_vote != server->vote_node.voted_for ||
             old_commit != server->vote_node.commit_index) &&
            durable_checkpoint(server) != 0)
            result = RAFT_ERR_INVALID_ARGUMENT;
        (void)pthread_mutex_unlock(&server->mutex);
        if (result == RAFT_OK && !rsp.success)
            raft_metrics_increment(&server->metrics, RAFT_METRIC_APPEND_REJECTIONS);
        if (result != RAFT_OK ||
            raft_append_encode_response(&rsp, output) != RAFT_OK) {
            fputs("Could not process AppendEntries\n", stderr);
            return;
        }
        status = raft_wire_send(fd, RAFT_WIRE_APPEND_ENTRIES_RESPONSE,
                                output, sizeof(output), message, sizeof(message));
        if (status == RAFT_WIRE_OK && batch.entry_count != 0) {
            printf("Log replication: follower=%u entries=%zu success=%d last_index=%llu\n",
                   server->node_id, batch.entry_count, rsp.success,
                   (unsigned long long)last);
            (void)fflush(stdout);
        }
        /* Empty heartbeat traffic is intentionally not logged. */
    } else if (status == RAFT_WIRE_OK && server->snapshot_mode &&
               type == RAFT_WIRE_INSTALL_SNAPSHOT) {
        raft_install_chunk_t chunk;
        uint64_t now_ms=0;
        if (raft_install_decode_chunk(payload,length,&chunk)!=RAFT_OK ||
            raft_config_peer_for_id(&server->config,chunk.leader_id)==NULL ||
            chunk.leader_id==server->node_id ||
            (raft_tls_enabled() && raft_tls_remote_node()!=chunk.leader_id) ||
            raft_clock_monotonic_ms(&now_ms)!=RAFT_OK) return;
        raft_install_response_t reply;
        uint8_t response[RAFT_INSTALL_RESPONSE_BYTES];
        uint8_t *complete=NULL; size_t complete_len=0;
        (void)pthread_mutex_lock(&server->mutex);
        raft_status_t result=raft_install_receive(&server->incoming,
            &server->vote_node,&server->kv,&chunk,now_ms,&reply,
            &complete,&complete_len);
        /* InstallSnapshot mutates the in-memory node. Never acknowledge the
         * completed installation until snapshot+suffix+metadata have reached
         * one durable generation. A failure is fatal to this process. */
        if (complete && server->snapshot_journal &&
            raft_snapwal_checkpoint(&server->snapwal, &server->vote_node,
                                    complete, complete_len, message,
                                    sizeof(message)) != 0) {
            fprintf(stderr, "FATAL snapshot installation persistence: %s\n", message);
            server->storage_failed=true;
            atomic_store(&server->election_stop,true);
            stop_requested=1;
            result=RAFT_ERR_INVALID_ARGUMENT;
            free(complete);
            complete=NULL;
        }
        if (!complete && result==RAFT_OK && server->snapshot_journal &&
            (server->vote_node.current_term != server->snapwal.previous.current_term ||
             server->vote_node.voted_for != server->snapwal.previous.voted_for) &&
            durable_checkpoint(server)!=0) result=RAFT_ERR_INVALID_ARGUMENT;
        if (complete) {
            raft_metrics_increment(&server->metrics, RAFT_METRIC_SNAPSHOTS_INSTALLED);
            free(server->snapshot_bytes);
            server->snapshot_bytes=complete;
            server->snapshot_length=complete_len;
            server->snapshot_index=chunk.last_index;
            server->snapshot_term=chunk.last_term;
            raft_index_t applied=0;
            (void)raft_apply_committed(&server->vote_node,&server->kv,&applied);
            printf("Snapshot installed: follower=%u boundary=%llu/%llu bytes=%zu\n",
                server->node_id,(unsigned long long)chunk.last_index,
                (unsigned long long)chunk.last_term,complete_len);
            (void)fflush(stdout);
        }
        (void)pthread_mutex_unlock(&server->mutex);
        if (result!=RAFT_OK ||
            raft_install_encode_response(&reply,response)!=RAFT_OK) return;
        (void)raft_wire_send(fd,RAFT_WIRE_INSTALL_SNAPSHOT_RESPONSE,response,
                             sizeof(response),message,sizeof(message));
    } else if (status == RAFT_WIRE_OK && server->snapshot_mode &&
               !server->auth_mode && type == RAFT_WIRE_SNAPSHOT_COMPACT_DEMO && length==0) {
        uint8_t response[17]={0};
        (void)pthread_mutex_lock(&server->mutex);
        raft_node_t *node=&server->vote_node;
        raft_snapshot_t shot;
        raft_snapshot_init(&shot);
        uint8_t *encoded=NULL; size_t encoded_len=0;
        int accepted=node->role==RAFT_LEADER && node->last_applied &&
            node->last_applied>node->log.base_index &&
            raft_snapshot_capture(node,&server->kv,&shot)==RAFT_OK &&
            raft_snapshot_encode(&shot,&encoded,&encoded_len)==RAFT_OK &&
            raft_snapshot_compact_applied(node,&shot)==RAFT_OK;
        if (accepted && server->snapshot_journal &&
            raft_snapwal_checkpoint(&server->snapwal, node, encoded,
                                    encoded_len, message, sizeof(message)) != 0) {
            fprintf(stderr, "FATAL snapshot checkpoint persistence: %s\n", message);
            server->storage_failed=true;
            atomic_store(&server->election_stop,true);
            stop_requested=1;
            accepted=0;
        }
        if (accepted) {
            free(server->snapshot_bytes);
            server->snapshot_bytes=encoded;
            server->snapshot_length=encoded_len;
            server->snapshot_index=shot.last_index;
            server->snapshot_term=shot.last_term;
            memset(server->snapshot_offsets,0,sizeof(server->snapshot_offsets));
            printf("Snapshot compacted: leader=%u boundary=%llu/%llu bytes=%zu\n",
                server->node_id,(unsigned long long)shot.last_index,
                (unsigned long long)shot.last_term,encoded_len);
            (void)fflush(stdout);
        } else free(encoded);
        raft_index_t index=node->log.base_index;
        raft_term_t term=node->log.base_term;
        (void)pthread_mutex_unlock(&server->mutex);
        raft_snapshot_destroy(&shot);
        for(unsigned b=0;b<8;b++) {
            response[b]=(uint8_t)(index>>(56u-8u*b));
            response[8+b]=(uint8_t)(term>>(56u-8u*b));
        }
        response[16]=(uint8_t)accepted;
        (void)raft_wire_send(fd,RAFT_WIRE_SNAPSHOT_COMPACT_RESPONSE,response,
                             sizeof(response),message,sizeof(message));
    } else if (status == RAFT_WIRE_OK && (type == RAFT_WIRE_CLIENT_WRITE_ID ||
               type == RAFT_WIRE_CLIENT_WRITE_AUTH)) {
        raft_dedup_entry_t request;
        raft_client_write_response_t reply={0,0,RAFT_CLIENT_WRITE_REJECTED};
        uint32_t timeout_ms=0;
        uint64_t start_ms=0;
        int valid=(type == RAFT_WIRE_CLIENT_WRITE_AUTH ?
               (server->auth_mode && raft_auth_verify(&server->auth_keys,payload,
                   length,&timeout_ms,&request)==0) :
               (!server->auth_mode && raft_dedup_decode_request(payload,length,
                   &timeout_ms,&request)==0)) &&
                  raft_clock_monotonic_ms(&start_ms)==RAFT_OK;
        if (valid) {
            (void)pthread_mutex_lock(&server->mutex);
            raft_node_t *node=&server->vote_node;
            reply.term=node->current_term;
            if (server->storage_failed) reply.status=RAFT_CLIENT_WRITE_UNKNOWN;
            else if (node->role!=RAFT_LEADER || !node->current_term)
                reply.status=RAFT_CLIENT_WRITE_NOT_LEADER;
            else if (stop_requested || atomic_load(&server->election_stop))
                reply.status=RAFT_CLIENT_WRITE_UNKNOWN;
            else {
                /* Apply any committed entries before looking up a request ID. */
                raft_index_t applied=0;
                if (raft_apply_committed(node,&server->kv,&applied)!=RAFT_OK)
                    reply.status=RAFT_CLIENT_WRITE_UNKNOWN;
                else reply.status=dedup_admit(server,&request,&reply.term,&reply.index);
            }
            (void)pthread_mutex_unlock(&server->mutex);
            while (reply.status==RAFT_CLIENT_WRITE_PENDING) {
                uint64_t now_ms=0;
                if (raft_clock_monotonic_ms(&now_ms)!=RAFT_OK) {
                    reply.status=RAFT_CLIENT_WRITE_UNKNOWN; break;
                }
                (void)pthread_mutex_lock(&server->mutex);
                reply.status=dedup_poll(server,reply.term,reply.index);
                (void)pthread_mutex_unlock(&server->mutex);
                if (reply.status!=RAFT_CLIENT_WRITE_PENDING) break;
                if (stop_requested || atomic_load(&server->election_stop)) {
                    reply.status=RAFT_CLIENT_WRITE_UNKNOWN; break;
                }
                if (now_ms-start_ms>=timeout_ms) {
                    reply.status=RAFT_CLIENT_WRITE_TIMEOUT; break;
                }
                struct timespec pause={.tv_sec=0,.tv_nsec=20000000L};
                (void)nanosleep(&pause,NULL);
            }
        }
        if (reply.status == RAFT_CLIENT_WRITE_COMMITTED)
            raft_metrics_increment(&server->metrics, RAFT_METRIC_CLIENT_COMMITS);
        if (reply.status == RAFT_CLIENT_WRITE_TIMEOUT)
            raft_metrics_increment(&server->metrics, RAFT_METRIC_CLIENT_TIMEOUTS);
        uint8_t out[RAFT_CLIENT_WRITE_RESPONSE_BYTES];
        if (raft_client_write_encode_response(&reply,out)==0)
            (void)raft_wire_send(fd,type==RAFT_WIRE_CLIENT_WRITE_AUTH ?
                                 RAFT_WIRE_CLIENT_WRITE_AUTH_RESPONSE :
                                 RAFT_WIRE_CLIENT_WRITE_ID_RESPONSE,
                                 out,sizeof(out),message,sizeof(message));
    } else if (status == RAFT_WIRE_OK && !server->auth_mode &&
               type == RAFT_WIRE_CLIENT_WRITE) {
        /* Bounded synchronous write: a reply says COMMITTED only once a
         * majority-backed index in this leader term is committed. Neither
         * local append nor an acknowledgement from one follower suffices.
         * Poll outside the mutex so the election thread can replicate. */
        raft_client_write_request_t request;
        raft_client_write_response_t reply = {0, 0, RAFT_CLIENT_WRITE_REJECTED};
        int valid = raft_client_write_decode_request(payload, length, &request) == 0;
        uint64_t start_ms = 0;
        if (valid && raft_clock_monotonic_ms(&start_ms) != RAFT_OK)
            valid = 0;
        if (valid) {
            (void)pthread_mutex_lock(&server->mutex);
            raft_node_t *node = &server->vote_node;
            reply.term = node->current_term;
            if (server->storage_failed) {
                reply.status = RAFT_CLIENT_WRITE_UNKNOWN;
            } else if (node->role != RAFT_LEADER || node->current_term == 0) {
                reply.status = RAFT_CLIENT_WRITE_NOT_LEADER;
            } else if (stop_requested || atomic_load(&server->election_stop)) {
                reply.status = RAFT_CLIENT_WRITE_UNKNOWN;
            } else if (raft_log_append(&node->log, node->current_term,
                                       request.command, request.command_len) != RAFT_OK) {
                reply.status = RAFT_CLIENT_WRITE_REJECTED;
            } else {
                reply.index = raft_log_last_index(&node->log);
                reply.status = durable_checkpoint(server) == 0 ?
                    RAFT_CLIENT_WRITE_PENDING : RAFT_CLIENT_WRITE_UNKNOWN;
            }
            (void)pthread_mutex_unlock(&server->mutex);
            while (reply.status == RAFT_CLIENT_WRITE_PENDING) {
                uint64_t now_ms = 0;
                if (raft_clock_monotonic_ms(&now_ms) != RAFT_OK) {
                    reply.status = RAFT_CLIENT_WRITE_UNKNOWN;
                    break;
                }
                (void)pthread_mutex_lock(&server->mutex);
                reply.status = server->storage_failed ? RAFT_CLIENT_WRITE_UNKNOWN :
                    raft_client_write_check(&server->vote_node,
                                            reply.term, reply.index);
                (void)pthread_mutex_unlock(&server->mutex);
                if (reply.status != RAFT_CLIENT_WRITE_PENDING) break;
                if (stop_requested || atomic_load(&server->election_stop)) {
                    reply.status = RAFT_CLIENT_WRITE_UNKNOWN;
                    break;
                }
                if (now_ms - start_ms >= request.timeout_ms) {
                    reply.status = RAFT_CLIENT_WRITE_TIMEOUT;
                    break;
                }
                struct timespec pause = {.tv_sec = 0, .tv_nsec = 20000000L};
                (void)nanosleep(&pause, NULL);
            }
        }
        if (reply.status == RAFT_CLIENT_WRITE_COMMITTED)
            raft_metrics_increment(&server->metrics, RAFT_METRIC_CLIENT_COMMITS);
        if (reply.status == RAFT_CLIENT_WRITE_TIMEOUT)
            raft_metrics_increment(&server->metrics, RAFT_METRIC_CLIENT_TIMEOUTS);
        uint8_t out[RAFT_CLIENT_WRITE_RESPONSE_BYTES];
        if (raft_client_write_encode_response(&reply, out) == 0)
            (void)raft_wire_send(fd, RAFT_WIRE_CLIENT_WRITE_RESPONSE, out,
                                 sizeof(out), message, sizeof(message));
    } else if (status == RAFT_WIRE_OK && !server->auth_mode &&
               type == RAFT_WIRE_DEMO_PROPOSE &&
               length != 0 && length <= 1024) {
        /* Explicitly educational, unauthenticated command injection.
         * Disabled for followers and --serve; never implies a commit. */
        uint8_t out[17] = {0};
        int accepted = 0;
        raft_index_t index = 0;
        raft_term_t term;
        (void)pthread_mutex_lock(&server->mutex);
        raft_node_t *node = &server->vote_node;
        term = node->current_term;
        if (node->role == RAFT_LEADER && term != 0 &&
            raft_log_append(&node->log, term, payload, length) == RAFT_OK) {
            accepted = 1;
            index = raft_log_last_index(&node->log);
            if (durable_checkpoint(server) != 0) {
                accepted = 0;
                index = 0;
            }
        }
        (void)pthread_mutex_unlock(&server->mutex);
        for (unsigned b = 0; b < 8; ++b) {
            out[b] = (uint8_t)(term >> (56u - 8u*b));
            out[8+b] = (uint8_t)(index >> (56u - 8u*b));
        }
        out[16] = (uint8_t)accepted;
        (void)raft_wire_send(fd, RAFT_WIRE_DEMO_PROPOSE_RESPONSE,
                             out, sizeof(out), message, sizeof(message));
        if (accepted) {
            printf("Demo proposal: leader=%u term=%llu index=%llu uncommitted\n",
                   server->node_id, (unsigned long long)term,
                   (unsigned long long)index);
            (void)fflush(stdout);
        }
    } else if (status == RAFT_WIRE_OK && type == RAFT_WIRE_LOG_STATUS_REQUEST &&
               length == 0) {
        /* Read-only diagnostics: last_index:u64, last_term:u64, commit:u64. */
        uint8_t out[24];
        raft_index_t index, committed;
        raft_term_t term = 0;
        (void)pthread_mutex_lock(&server->mutex);
        index = raft_log_last_index(&server->vote_node.log);
        committed = server->vote_node.commit_index;
        (void)raft_log_term_at(&server->vote_node.log, index, &term);
        (void)pthread_mutex_unlock(&server->mutex);
        uint64_t numbers[] = {index, term, committed};
        for (size_t i = 0; i < 3; ++i)
            for (unsigned b = 0; b < 8; ++b)
                out[i * 8 + b] = (uint8_t)(numbers[i] >> (56u - 8u*b));
        (void)raft_wire_send(fd, RAFT_WIRE_LOG_STATUS_RESPONSE,
                             out, sizeof(out), message, sizeof(message));
    } else if (status == RAFT_WIRE_OK && type == RAFT_WIRE_APPLY_STATUS_REQUEST &&
               length == 0) {
        /* lastApplied:u64, commitIndex:u64, keyCount:u32, ignored:u64. */
        uint8_t out[28] = {0};
        uint64_t applied, committed, ignored;
        uint32_t keys;
        (void)pthread_mutex_lock(&server->mutex);
        applied = server->vote_node.last_applied;
        committed = server->vote_node.commit_index;
        keys = (uint32_t)server->kv.count;
        ignored = server->kv.ignored_commands;
        (void)pthread_mutex_unlock(&server->mutex);
        uint64_t numbers[] = {applied, committed, ignored};
        for (size_t i = 0; i < 2; ++i)
            for (unsigned b = 0; b < 8; ++b)
                out[i*8+b] = (uint8_t)(numbers[i] >> (56u-8u*b));
        for (unsigned b = 0; b < 4; ++b)
            out[16+b] = (uint8_t)(keys >> (24u-8u*b));
        for (unsigned b = 0; b < 8; ++b)
            out[20+b] = (uint8_t)(numbers[2] >> (56u-8u*b));
        (void)raft_wire_send(fd, RAFT_WIRE_APPLY_STATUS_RESPONSE,
                             out, sizeof(out), message, sizeof(message));
    } else if (status == RAFT_WIRE_OK && type == RAFT_WIRE_DEMO_GET_REQUEST &&
               raft_kv_valid_key(payload, length)) {
        /* Local diagnostic read only; cannot provide linearizability. */
        uint8_t out[11 + RAFT_KV_MAX_VALUE_BYTES] = {0};
        const uint8_t *value = NULL;
        size_t value_len = 0;
        (void)pthread_mutex_lock(&server->mutex);
        int found = raft_kv_get(&server->kv, payload, length, &value, &value_len);
        raft_index_t applied = server->vote_node.last_applied;
        if (found == 1 && value_len <= RAFT_KV_MAX_VALUE_BYTES) {
            out[0] = 1;
            out[9] = (uint8_t)(value_len >> 8);
            out[10] = (uint8_t)value_len;
            if (value_len) memcpy(out + 11, value, value_len);
        }
        for (unsigned b = 0; b < 8; ++b)
            out[1+b] = (uint8_t)(applied >> (56u-8u*b));
        (void)pthread_mutex_unlock(&server->mutex);
        if (found >= 0)
            (void)raft_wire_send(fd, RAFT_WIRE_DEMO_GET_RESPONSE,
                                 out, 11 + (out[0] ? value_len : 0),
                                 message, sizeof(message));
    } else if (status == RAFT_WIRE_OK && type == RAFT_WIRE_STATUS_REQUEST &&
               length == 0) {
        /* Read-only debug status: node_id:u32 term:u64 role:u8 voted_for:u32. */
        uint8_t out[17];
        (void)pthread_mutex_lock(&server->mutex);
        raft_node_id_t id = server->vote_node.id;
        raft_term_t term = server->vote_node.current_term;
        raft_role_t role = server->vote_node.role;
        raft_node_id_t voted = server->vote_node.voted_for;
        (void)pthread_mutex_unlock(&server->mutex);
        for (int i = 0; i < 4; ++i) {
            out[i] = (uint8_t)(id >> (24 - i * 8));
            out[13 + i] = (uint8_t)(voted >> (24 - i * 8));
        }
        for (int i = 0; i < 8; ++i)
            out[4 + i] = (uint8_t)(term >> (56 - i * 8));
        out[12] = (uint8_t)role;
        (void)raft_wire_send(fd, RAFT_WIRE_STATUS_RESPONSE,
                             out, sizeof(out), message, sizeof(message));
    } else if (status == RAFT_WIRE_OK) {
        raft_metrics_increment(&server->metrics, RAFT_METRIC_REJECTED_FRAMES);
        fputs("Rejected frame: unsupported or malformed request\n", stderr);
    }
    if (status != RAFT_WIRE_OK && status != RAFT_WIRE_CLOSED)
        fprintf(stderr, "Rejected frame: %s\n", message);
}

static void *tcp_worker_main(void *arg)
{
    tcp_worker_t *worker = arg;
    tcp_server_t *server = worker->server;
    int fd = worker->client_fd;
    char tls_error[256] = {0};
    if (configure_client_timeouts(fd) == 0 &&
        raft_tls_attach_inbound(fd, tls_error, sizeof(tls_error)) == 0)
        process_client(fd, server);
    else if (raft_tls_enabled()) {
        raft_metrics_increment(&server->metrics, RAFT_METRIC_TLS_HANDSHAKE_FAILURES);
        fprintf(stderr, "TLS connection rejected: %s\n", tls_error);
    }
    else
        fprintf(stderr, "Client socket timeout setup failed: %s\n", strerror(errno));
    raft_tls_detach();
    raft_metrics_connection_close(&server->metrics);

    /* Closing and clearing the fd under one mutex prevents fd-reuse races
     * with the main thread's concurrent shutdown() on SIGTERM/SIGINT. */
    (void)pthread_mutex_lock(&server->mutex);
    (void)close(fd);
    worker->client_fd = -1;
    worker->finished = true;
    (void)pthread_cond_signal(&server->worker_changed);
    (void)pthread_mutex_unlock(&server->mutex);
    return NULL;
}

static int tcp_server_init(tcp_server_t *server, raft_node_id_t node_id)
{
    memset(server, 0, sizeof(*server));
    server->node_id = node_id;
    uint64_t started_ms = 0;
    (void)raft_clock_monotonic_ms(&started_ms);
    raft_metrics_init(&server->metrics, started_ms);
    raft_kv_init(&server->kv);
    raft_install_receiver_init(&server->incoming);
    atomic_init(&server->election_stop, false);
    server->storage.directory_fd = -1;
    server->storage.lock_fd = -1;
    server->snapwal.fd = -1;
    server->snapwal.lock.directory_fd = -1;
    server->snapwal.lock.lock_fd = -1;
    server->wal.fd = -1;
    server->wal.lock.directory_fd = -1;
    server->wal.lock.lock_fd = -1;
    int code = pthread_mutex_init(&server->mutex, NULL);
    if (code != 0) return code;
    code = pthread_cond_init(&server->worker_changed, NULL);
    if (code != 0) {
        (void)pthread_mutex_destroy(&server->mutex);
        return code;
    }
    if (raft_node_init(&server->vote_node, node_id) != RAFT_OK) {
        (void)pthread_cond_destroy(&server->worker_changed);
        (void)pthread_mutex_destroy(&server->mutex);
        return EINVAL;
    }
    uint64_t now_ms = 0;
    if (raft_clock_monotonic_ms(&now_ms) != RAFT_OK ||
        raft_node_start_follower(&server->vote_node, now_ms,
                                 0x9E3779B9U ^ node_id ^ (uint32_t)getpid() ^ (uint32_t)now_ms) != RAFT_OK) {
        raft_node_destroy(&server->vote_node);
        (void)pthread_cond_destroy(&server->worker_changed);
        (void)pthread_mutex_destroy(&server->mutex);
        return EINVAL;
    }
    return 0;
}

/* Only the accept thread reaps/joins. Never reuse a pthread_t until joined. */
static void reap_finished_workers(tcp_server_t *server)
{
    for (size_t i = 0; i < RAFT_TCP_MAX_WORKERS; ++i) {
        tcp_worker_t *worker = &server->workers[i];
        (void)pthread_mutex_lock(&server->mutex);
        bool finished = worker->in_use && worker->finished;
        (void)pthread_mutex_unlock(&server->mutex);
        if (finished) {
            (void)pthread_join(worker->thread, NULL);
            /* Worker no longer accesses this slot. */
            worker->in_use = false;
            worker->finished = false;
        }
    }
}

static tcp_worker_t *free_worker_slot(tcp_server_t *server)
{
    for (size_t i = 0; i < RAFT_TCP_MAX_WORKERS; ++i)
        if (!server->workers[i].in_use) return &server->workers[i];
    return NULL;
}

static void wait_for_worker(tcp_server_t *server)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) return;
    deadline.tv_nsec += RAFT_TCP_SLOT_WAIT_MS * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    (void)pthread_mutex_lock(&server->mutex);
    /* A worker may finish just before timedwait; at worst wait 100ms. */
    (void)pthread_cond_timedwait(&server->worker_changed, &server->mutex, &deadline);
    (void)pthread_mutex_unlock(&server->mutex);
}

static void stop_and_join_workers(tcp_server_t *server, bool interrupt)
{
    atomic_store(&server->election_stop, true);
    if (server->election_thread_started)
        (void)pthread_join(server->election_thread, NULL);
    if (interrupt) {
        (void)pthread_mutex_lock(&server->mutex);
        for (size_t i = 0; i < RAFT_TCP_MAX_WORKERS; ++i) {
            tcp_worker_t *worker = &server->workers[i];
            if (worker->in_use && worker->client_fd >= 0)
                (void)shutdown(worker->client_fd, SHUT_RDWR);
        }
        (void)pthread_mutex_unlock(&server->mutex);
    }
    for (size_t i = 0; i < RAFT_TCP_MAX_WORKERS; ++i) {
        if (server->workers[i].in_use) {
            (void)pthread_join(server->workers[i].thread, NULL);
            server->workers[i].in_use = false;
        }
    }
    if (server->snapshot_journal) raft_snapwal_close(&server->snapwal);
    if (server->journal) raft_wal_close(&server->wal);
    raft_storage_close(&server->storage);
    /* No worker may still access credentials once joined. */
    raft_auth_clear_keys(&server->auth_keys);
    raft_install_receiver_destroy(&server->incoming);
    free(server->snapshot_bytes);
    raft_kv_destroy(&server->kv);
    raft_node_destroy(&server->vote_node);
    (void)pthread_cond_destroy(&server->worker_changed);
    (void)pthread_mutex_destroy(&server->mutex);
}

/* One outbound vote request. Socket I/O is deliberately OUTSIDE the
 * node mutex so inbound peer voting continues during elections. */
static int request_peer_vote(const raft_peer_config_t *peer,
                             const raft_vote_request_t *req,
                             raft_vote_response_t *response)
{
    char error[256];
    int fd = raft_tcp_connect(peer, 250, error, sizeof(error));
    if (fd < 0) return -1;
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 250000};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        (void)close(fd);
        return -1;
    }
    uint8_t payload[RAFT_VOTE_REQUEST_BYTES];
    if (raft_vote_encode_request(req, payload) != RAFT_OK) {
        (void)close(fd);
        return -1;
    }
    raft_wire_status_t result = raft_wire_send(fd, RAFT_WIRE_REQUEST_VOTE,
                               payload, sizeof(payload), error, sizeof(error));
    uint8_t reply[RAFT_VOTE_RESPONSE_BYTES];
    raft_wire_type_t type = RAFT_WIRE_PING;
    size_t length = 0;
    if (result == RAFT_WIRE_OK)
        result = raft_wire_recv(fd, &type, reply, sizeof(reply), &length,
                                error, sizeof(error));
    (void)close(fd);
    return result == RAFT_WIRE_OK && type == RAFT_WIRE_REQUEST_VOTE_RESPONSE &&
           raft_vote_decode_response(reply, length, response) == RAFT_OK ? 0 : -1;
}


/* Phase 13: send either entry-bearing batches or empty heartbeats.
 * Encode while holding the mutex, and perform TCP I/O outside it. */
#define RAFT_HEARTBEAT_INTERVAL_MS 90u
#define RAFT_HEARTBEAT_CONNECT_MS 80
#define RAFT_HEARTBEAT_IO_TIMEOUT_US 95000

static int send_peer_replication(const raft_peer_config_t *peer,
                                 const uint8_t *payload, size_t length,
                                 raft_append_response_t *response)
{
    char error[256];
    int fd = raft_tcp_connect(peer, RAFT_HEARTBEAT_CONNECT_MS, error, sizeof(error));
    if (fd < 0) return -1;
    struct timeval timeout = {.tv_sec = 0, .tv_usec = RAFT_HEARTBEAT_IO_TIMEOUT_US};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        (void)close(fd);
        return -1;
    }
    raft_wire_status_t result = raft_wire_send(fd, RAFT_WIRE_APPEND_ENTRIES,
                                payload, length, error, sizeof(error));
    uint8_t reply[RAFT_APPEND_RESPONSE_BYTES];
    raft_wire_type_t type = RAFT_WIRE_PING;
    size_t reply_length = 0;
    if (result == RAFT_WIRE_OK)
        result = raft_wire_recv(fd, &type, reply, sizeof(reply), &reply_length,
                                error, sizeof(error));
    (void)close(fd);
    return result == RAFT_WIRE_OK && type == RAFT_WIRE_APPEND_ENTRIES_RESPONSE &&
           raft_append_decode_response(reply, reply_length, response) == RAFT_OK ? 0 : -1;
}

/* Each chunk is its own framed request on a new short-lived TCP connection.
 * The follower returns the next expected offset to recover lost acknowledgements. */
static int send_peer_snapshot(const raft_peer_config_t *peer,
                              const uint8_t *payload, size_t length,
                              raft_install_response_t *response)
{
    char error[256];
    int fd=raft_tcp_connect(peer,RAFT_HEARTBEAT_CONNECT_MS,error,sizeof(error));
    if (fd<0) return -1;
    struct timeval timeout={.tv_sec=0,.tv_usec=RAFT_HEARTBEAT_IO_TIMEOUT_US};
    if (setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))!=0 ||
        setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout))!=0) {
        (void)close(fd); return -1;
    }
    raft_wire_status_t result=raft_wire_send(fd,RAFT_WIRE_INSTALL_SNAPSHOT,
                                payload,length,error,sizeof(error));
    uint8_t reply[RAFT_INSTALL_RESPONSE_BYTES];
    raft_wire_type_t type=RAFT_WIRE_PING; size_t reply_length=0;
    if (result==RAFT_WIRE_OK)
        result=raft_wire_recv(fd,&type,reply,sizeof(reply),&reply_length,
                              error,sizeof(error));
    (void)close(fd);
    return result==RAFT_WIRE_OK && type==RAFT_WIRE_INSTALL_SNAPSHOT_RESPONSE &&
        raft_install_decode_response(reply,reply_length,response)==RAFT_OK ? 0 : -1;
}

static void *election_worker_main(void *argument)
{
    tcp_server_t *server = argument;
    const struct timespec interval = {.tv_sec = 0, .tv_nsec = 20000000L};
    uint64_t next_heartbeat_ms = 0;
    while (!atomic_load(&server->election_stop) && !stop_requested) {
        uint64_t now = 0;
        if (raft_clock_monotonic_ms(&now) != RAFT_OK) break;
        raft_election_round_t round;
        raft_vote_request_t request;
        bool begin = false;
        bool heartbeat = false;
        raft_term_t heartbeat_term = 0;
        (void)pthread_mutex_lock(&server->mutex);
        raft_node_t *node = &server->vote_node;
        if (node->role != RAFT_LEADER &&
            raft_election_timer_expired(&node->election_timer, now) &&
            raft_election_begin(node, now, &round) == RAFT_OK) {
            raft_index_t index = raft_log_last_index(&node->log);
            raft_term_t log_term = 0;
            (void)raft_log_term_at(&node->log, index, &log_term);
            request = (raft_vote_request_t){node->current_term, node->id,
                                            index, log_term};
            begin = durable_checkpoint(server) == 0;
            if (begin) raft_metrics_increment(&server->metrics, RAFT_METRIC_ELECTION_ROUNDS);
            if (begin) printf("Election started: node=%u term=%llu self_votes=1\n",
                   node->id, (unsigned long long)node->current_term);
            (void)fflush(stdout);
        }
        if (node->role == RAFT_LEADER && now >= next_heartbeat_ms) {
            heartbeat_term = node->current_term;
            heartbeat = true;
            next_heartbeat_ms = now + RAFT_HEARTBEAT_INTERVAL_MS;
        }
        (void)pthread_mutex_unlock(&server->mutex);
        if (heartbeat) {
            for (size_t i = 0; i < server->config.peer_count; ++i) {
                if (atomic_load(&server->election_stop) || stop_requested) break;
                const raft_peer_config_t *peer = &server->config.peers[i];
                if (peer->id == server->node_id) continue;
                uint8_t payload[RAFT_WIRE_MAX_PAYLOAD];
                raft_append_batch_t batch;
                raft_index_t sent_through = 0;
                size_t payload_length = 0;
                size_t entries=0;
                bool sending_snapshot=false;
                raft_index_t selected_index=0;
                uint32_t selected_offset=0, selected_total=0, sent_bytes=0;
                (void)pthread_mutex_lock(&server->mutex);
                bool valid = server->vote_node.role == RAFT_LEADER &&
                    server->vote_node.current_term == heartbeat_term &&
                    server->progress[i].term == heartbeat_term &&
                    server->progress[i].peer_id == peer->id;
                if (valid && server->snapshot_mode && server->snapshot_bytes &&
                    server->progress[i].next_index<=server->vote_node.log.base_index) {
                    sending_snapshot=true;
                    selected_index=server->snapshot_index;
                    selected_total=(uint32_t)server->snapshot_length;
                    selected_offset=server->snapshot_offsets[i];
                    if (selected_offset>=selected_total) selected_offset=0;
                    size_t remaining=selected_total-selected_offset;
                    sent_bytes=(uint32_t)(remaining>RAFT_INSTALL_CHUNK_BYTES ?
                                             RAFT_INSTALL_CHUNK_BYTES : remaining);
                    raft_install_chunk_t chunk={heartbeat_term,server->node_id,
                        selected_index,server->snapshot_term,selected_offset,
                        selected_total,selected_offset+sent_bytes==selected_total,
                        server->snapshot_bytes+selected_offset,sent_bytes};
                    valid=raft_install_encode_chunk(&chunk,payload,sizeof(payload),
                                                     &payload_length)==RAFT_OK;
                } else if (valid) {
                    valid=raft_replication_prepare(&server->vote_node,
                        &server->progress[i],&batch,&sent_through)==RAFT_OK &&
                        raft_append_encode_batch(&batch,payload,sizeof(payload),
                                                 &payload_length)==RAFT_OK;
                    entries=valid ? batch.entry_count : 0;
                }
                (void)pthread_mutex_unlock(&server->mutex);
                if (!valid) break;
                raft_append_response_t reply;
                raft_install_response_t snap_reply={0};
                if (sending_snapshot) {
                    if (send_peer_snapshot(peer,payload,payload_length,&snap_reply)!=0) {
                        raft_metrics_increment(&server->metrics, RAFT_METRIC_REPLICATION_RPC_FAILURES);
                        continue;
                    }
                    reply=(raft_append_response_t){snap_reply.term,snap_reply.accepted};
                } else if (send_peer_replication(peer,payload,payload_length,&reply)!=0) {
                    raft_metrics_increment(&server->metrics, RAFT_METRIC_REPLICATION_RPC_FAILURES);
                    continue; /* network failure retains nextIndex for retry */
                }
                raft_metrics_increment(&server->metrics, RAFT_METRIC_REPLICATION_RPC_SUCCESSES);
                if (!sending_snapshot && reply.success &&
                    reply.term == heartbeat_term && entries)
                    raft_metrics_add(&server->metrics, RAFT_METRIC_ENTRIES_SENT, entries);
                if (raft_clock_monotonic_ms(&now) != RAFT_OK) break;
                (void)pthread_mutex_lock(&server->mutex);
                raft_role_t old = server->vote_node.role;
                raft_term_t old_term = server->vote_node.current_term;
                raft_index_t old_commit = server->vote_node.commit_index;
                (void)raft_append_handle_response(&server->vote_node, &reply, now);
                bool still_leader = server->vote_node.role == RAFT_LEADER &&
                    server->vote_node.current_term == heartbeat_term;
                if (still_leader && reply.success && reply.term == heartbeat_term) {
                    /* One other voter + self is a quorum for this fixed
                     * three-member cluster. This is recent contact, NOT a lease. */
                    server->health_last_ack_ms = now;
                    server->health_last_ack_term = heartbeat_term;
                    server->health_ack_valid = true;
                }
                if (still_leader) {
                    if (sending_snapshot) {
                        if (selected_index==server->snapshot_index &&
                            selected_total==server->snapshot_length &&
                            snap_reply.next_offset<=selected_total &&
                            (!snap_reply.accepted ||
                             (snap_reply.next_offset==selected_offset+sent_bytes ||
                              snap_reply.next_offset==selected_total))) {
                            server->snapshot_offsets[i]=snap_reply.next_offset;
                            if (snap_reply.accepted &&
                                snap_reply.next_offset==selected_total) {
                                if (server->progress[i].match_index<selected_index)
                                    server->progress[i].match_index=selected_index;
                                if (server->progress[i].next_index<selected_index+1)
                                    server->progress[i].next_index=selected_index+1;
                                printf("Snapshot acknowledged: leader=%u follower=%u boundary=%llu\n",
                                    server->node_id,peer->id,
                                    (unsigned long long)selected_index);
                                (void)fflush(stdout);
                            }
                        } else server->snapshot_offsets[i]=0;
                    } else {
                        (void)raft_replication_observe(&server->progress[i],
                                                       sent_through,&reply);
                        /* A complete checkpoint supersedes retrying already
                         * discarded prefixes. Install it on an older peer
                         * rather than waiting through O(prefix_length)
                         * failed prevIndex probes and follower elections. */
                        if (server->snapshot_mode && server->snapshot_bytes &&
                            !reply.success &&
                            server->progress[i].next_index > server->snapshot_index)
                            server->progress[i].next_index=server->snapshot_index;
                    }
                    /* Successful same-term AppendEntries acknowledgements
                     * may move the majority-backed commit index. All node
                     * and peer progress fields remain under the same mutex. */
                    raft_replication_progress_t followers[RAFT_CLUSTER_SIZE - 1];
                    size_t follower_count = 0;
                    for (size_t j = 0; j < server->config.peer_count; ++j) {
                        if (server->config.peers[j].id == server->node_id) continue;
                        followers[follower_count++] = server->progress[j];
                    }
                    raft_index_t previous_commit = server->vote_node.commit_index;
                    raft_index_t new_commit = previous_commit;
                    if (raft_commit_advance(&server->vote_node, followers,
                                             follower_count, &new_commit) == RAFT_OK &&
                        new_commit > previous_commit) {
                        printf("Commit advanced: leader=%u term=%llu commitIndex=%llu\n",
                               server->node_id, (unsigned long long)heartbeat_term,
                               (unsigned long long)new_commit);
                        (void)fflush(stdout);
                    }
                    /* Application follows commitment, never mere replication.
                     * OOM retains lastApplied and is retried every heartbeat. */
                    raft_index_t applied = 0;
                    raft_status_t apply_status = raft_apply_committed(
                        &server->vote_node, &server->kv, &applied);
                    if (apply_status != RAFT_OK)
                        fputs("Leader state-machine application deferred\n", stderr);
                    if (entries && reply.success) {
                        printf("Replicated: leader=%u follower=%u matchIndex=%llu nextIndex=%llu\n",
                               server->node_id, peer->id,
                               (unsigned long long)server->progress[i].match_index,
                               (unsigned long long)server->progress[i].next_index);
                        (void)fflush(stdout);
                    }
                }
                if (old == RAFT_LEADER && !still_leader) {
                    printf("Leader stepped down: node=%u term=%llu\n", server->node_id,
                           (unsigned long long)server->vote_node.current_term);
                    (void)fflush(stdout);
                }
                if ((server->vote_node.current_term != old_term ||
                     server->vote_node.commit_index != old_commit) &&
                    durable_checkpoint(server) != 0) still_leader = false;
                (void)pthread_mutex_unlock(&server->mutex);
                if (!still_leader) break;
            }
        }
        if (begin) {
            for (size_t i = 0; i < server->config.peer_count; ++i) {
                if (atomic_load(&server->election_stop) || stop_requested) break;
                const raft_peer_config_t *peer = &server->config.peers[i];
                if (peer->id == server->node_id) continue;
                raft_vote_response_t reply;
                if (request_peer_vote(peer, &request, &reply) != 0) continue;
                if (raft_clock_monotonic_ms(&now) != RAFT_OK) break;
                (void)pthread_mutex_lock(&server->mutex);
                raft_role_t previous = server->vote_node.role;
                raft_term_t old_term = server->vote_node.current_term;
                raft_node_id_t old_vote = server->vote_node.voted_for;
                (void)raft_election_record(&server->vote_node, &round,
                                            peer->id, &reply, now);
                if (previous == RAFT_CANDIDATE &&
                    server->vote_node.role == RAFT_LEADER) {
                    raft_metrics_increment(&server->metrics, RAFT_METRIC_LEADER_TRANSITIONS);
                    server->health_ack_valid = false;
                    printf("Leader elected: node=%u term=%llu votes=%u/3\n",
                           server->node_id,
                           (unsigned long long)server->vote_node.current_term,
                           round.votes);
                    (void)fflush(stdout);
                    raft_index_t last = raft_log_last_index(&server->vote_node.log);
                    for (size_t j = 0; j < server->config.peer_count; ++j) {
                        raft_node_id_t id = server->config.peers[j].id;
                        if (id == server->node_id) continue;
                        (void)raft_replication_progress_init(&server->progress[j],
                              id, server->vote_node.current_term, last);
                    }
                    memset(server->snapshot_offsets,0,sizeof(server->snapshot_offsets));
                    next_heartbeat_ms = 0; /* send first AppendEntries next loop */
                }
                bool continuing = server->vote_node.role == RAFT_CANDIDATE &&
                                  server->vote_node.current_term == round.term;
                if ((server->vote_node.current_term != old_term ||
                     server->vote_node.voted_for != old_vote) &&
                    durable_checkpoint(server) != 0) continuing = false;
                (void)pthread_mutex_unlock(&server->mutex);
                if (!continuing) break;
            }
        }
        (void)nanosleep(&interval, NULL);
    }
    return NULL;
}

static int tcp_serve_mode(const raft_cluster_config_t *config,
                          unsigned int max_connections, bool elections,
                          const char *durable_directory, bool journal, bool snapshots,
                          bool snapshot_journal, const char *auth_keyring,
                          char *error, size_t error_capacity)
{
    if (raft_config_validate(config, error, error_capacity) != RAFT_CONFIG_OK)
        return -1;
    if (raft_tls_validate_local_node(config->node_id) != 0) {
        report_error(error, error_capacity,
                     "local TLS certificate SAN does not match configured node ID");
        return -1;
    }
    const raft_peer_config_t *local =
        raft_config_peer_for_id(config, config->node_id);
    int listener = raft_tcp_listen(local, error, error_capacity);
    if (listener < 0) return -1;

    tcp_server_t server;
    int code = tcp_server_init(&server, config->node_id);
    if (code == 0) {
        server.config = *config;
        server.snapshot_mode = snapshots;
        server.snapshot_journal = snapshot_journal;
    }
    if (code != 0) {
        report_error(error, error_capacity, "pthread server initialization: %s", strerror(code));
        (void)close(listener);
        return -1;
    }
    if (auth_keyring != NULL) {
        if (raft_auth_read_keyring(auth_keyring,&server.auth_keys)!=0) {
            report_error(error,error_capacity,
                         "invalid auth keyring (owner-only regular file required)");
            (void)close(listener);
            stop_and_join_workers(&server,false);
            return -1;
        }
        server.auth_mode=true;
    }
    if (durable_directory != NULL) {
        if (snapshot_journal &&
            raft_snapwal_open(&server.snapwal,durable_directory,config->node_id,
                              error,error_capacity)!=0) {
            (void)close(listener);
            /* The open helper closed its partially initialized descriptor. */
            server.snapshot_journal=false;
            stop_and_join_workers(&server,false);
            return -1;
        }
        if (journal) {
            if (raft_wal_open(&server.wal,durable_directory,config->node_id,
                              error,error_capacity)!=0) {
                (void)close(listener);
                stop_and_join_workers(&server,false);
                return -1;
            }
            server.journal=true;
        }
        if (!journal && !snapshot_journal && raft_storage_open(&server.storage, durable_directory, config->node_id,
                              error, error_capacity) != 0) {
            (void)close(listener);
            stop_and_join_workers(&server, false);
            return -1;
        }
        server.durable = !journal && !snapshot_journal;
        int found = 0;
        uint8_t *restored_snapshot=NULL;
        size_t restored_length=0;
        if ((snapshot_journal ?
             raft_snapwal_load(&server.snapwal, &server.vote_node, &found,
                               &restored_snapshot, &restored_length,
                               error,error_capacity) :
             (journal ? raft_wal_load(&server.wal,&server.vote_node,&found,
                                     error,error_capacity) :
              raft_storage_load(&server.storage, &server.vote_node, &found,
                               error, error_capacity))) != 0) {
            (void)close(listener);
            stop_and_join_workers(&server, false);
            return -1; /* corrupt durable state must NOT be replaced */
        }
        if (snapshot_journal && restored_length) {
            raft_snapshot_t snapshot;
            raft_snapshot_init(&snapshot);
            raft_node_t fresh;
            (void)raft_node_init(&fresh,server.node_id);
            fresh.current_term=server.vote_node.current_term;
            int ok=raft_snapshot_decode(restored_snapshot,restored_length,
                                        &snapshot)==RAFT_OK &&
                   snapshot.last_index==server.vote_node.log.base_index &&
                   snapshot.last_term==server.vote_node.log.base_term &&
                   raft_snapshot_restore_fresh(&snapshot,&fresh,&server.kv)==RAFT_OK;
            if (ok) server.vote_node.last_applied=snapshot.last_index;
            raft_node_destroy(&fresh);
            raft_snapshot_destroy(&snapshot);
            if (!ok) {
                report_error(error,error_capacity,"snapshot WAL recovery state mismatch");
                free(restored_snapshot);
                (void)close(listener);
                stop_and_join_workers(&server,false);
                return -1;
            }
            server.snapshot_bytes=restored_snapshot;
            server.snapshot_length=restored_length;
            server.snapshot_index=server.vote_node.log.base_index;
            server.snapshot_term=server.vote_node.log.base_term;
        } else free(restored_snapshot);
        uint64_t now = 0;
        raft_index_t replayed = 0;
        if (raft_clock_monotonic_ms(&now) != RAFT_OK ||
            raft_node_start_follower(&server.vote_node, now,
                     0x9E3779B9u ^ config->node_id ^ (uint32_t)getpid() ^ (uint32_t)now) != RAFT_OK ||
            raft_apply_committed(&server.vote_node, &server.kv, &replayed) != RAFT_OK ||
            (!found && !journal && !snapshot_journal && raft_storage_save(&server.storage, &server.vote_node,
                                         error, error_capacity) != 0)) {
            if (error && error_capacity && error[0] == '\0')
                report_error(error, error_capacity, "could not restore durable state");
            (void)close(listener);
            stop_and_join_workers(&server, false);
            return -1;
        }
        printf("%s recovery: node=%u restored=%d term=%llu log=%llu commit=%llu\n",
               snapshot_journal ? "Snapshot-WAL" : (journal ? "WAL" : "Durable"),
               config->node_id, found,
               (unsigned long long)server.vote_node.current_term,
               (unsigned long long)raft_log_last_index(&server.vote_node.log),
               (unsigned long long)server.vote_node.commit_index);
        (void)fflush(stdout);
    }

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_stop_signal;
    (void)sigemptyset(&action.sa_mask);
    struct sigaction previous_int, previous_term;
    stop_requested = 0;
    if (sigaction(SIGINT, &action, &previous_int) < 0) {
        report_error(error, error_capacity, "install SIGINT handler: %s", strerror(errno));
        (void)close(listener);
        stop_and_join_workers(&server, false);
        return -1;
    }
    if (sigaction(SIGTERM, &action, &previous_term) < 0) {
        report_error(error, error_capacity, "install SIGTERM handler: %s", strerror(errno));
        (void)sigaction(SIGINT, &previous_int, NULL);
        (void)close(listener);
        stop_and_join_workers(&server, false);
        return -1;
    }

    struct sigaction previous_hup;
    bool hup_installed = false;
    tls_reload_requested = 0;
    if (raft_tls_reload_enabled()) {
        struct sigaction hup_action;
        memset(&hup_action, 0, sizeof(hup_action));
        hup_action.sa_handler = on_reload_signal;
        (void)sigemptyset(&hup_action.sa_mask);
        if (sigaction(SIGHUP, &hup_action, &previous_hup) != 0) {
            report_error(error, error_capacity, "install SIGHUP handler: %s", strerror(errno));
            (void)close(listener);
            stop_and_join_workers(&server, false);
            (void)sigaction(SIGTERM, &previous_term, NULL);
            (void)sigaction(SIGINT, &previous_int, NULL);
            return -1;
        }
        hup_installed = true;
    }

    if (elections) {
        code = pthread_create(&server.election_thread, NULL,
                              election_worker_main, &server);
        if (code != 0) {
            report_error(error, error_capacity, "election worker: %s", strerror(code));
            (void)close(listener);
            stop_and_join_workers(&server, false);
            if (hup_installed) (void)sigaction(SIGHUP, &previous_hup, NULL);
            (void)sigaction(SIGTERM, &previous_term, NULL);
            (void)sigaction(SIGINT, &previous_int, NULL);
            return -1;
        }
        server.election_thread_started = true;
    }
    unsigned int accepted = 0;
    int result = 0;
    printf("Listening: node=%u %s:%u (max_connections=%u workers=%u)\n",
           config->node_id, local->ipv4, (unsigned)local->port,
           max_connections, RAFT_TCP_MAX_WORKERS);
    (void)fflush(stdout);

    while (!stop_requested &&
           (max_connections == 0 || accepted < max_connections)) {
        if (tls_reload_requested) {
            tls_reload_requested = 0;
            char reload_error[256] = {0};
            if (raft_tls_reload(reload_error, sizeof(reload_error)) == 0) {
                raft_metrics_increment(&server.metrics, RAFT_METRIC_TLS_RELOAD_SUCCESSES);
                fprintf(stderr, "TLS credentials reloaded for node %u\n", config->node_id);
            } else {
                raft_metrics_increment(&server.metrics, RAFT_METRIC_TLS_RELOAD_FAILURES);
                fprintf(stderr, "TLS reload rejected for node %u: %s\n",
                        config->node_id, reload_error);
            }
        }
        reap_finished_workers(&server);
        if (free_worker_slot(&server) == NULL) {
            wait_for_worker(&server);  /* backpressure instead of unbounded threads */
            continue;
        }
        struct pollfd ready = {.fd = listener, .events = POLLIN, .revents = 0};
        int n = poll(&ready, 1, 100);
        if (n < 0) {
            if (errno == EINTR) continue;
            report_error(error, error_capacity, "poll(listener): %s", strerror(errno));
            result = -1;
            break;
        }
        if (n == 0) continue;
        if (!(ready.revents & POLLIN)) {
            report_error(error, error_capacity, "listener received poll error flags: 0x%x",
                         (unsigned)ready.revents);
            result = -1;
            break;
        }
        struct sockaddr_in remote;
        socklen_t remote_len = sizeof(remote);
        int client = accept(listener, (struct sockaddr *)&remote, &remote_len);
        if (client < 0) {
            if (errno == EINTR) continue;
            report_error(error, error_capacity, "accept: %s", strerror(errno));
            result = -1;
            break;
        }
        ++accepted;
        raft_metrics_increment(&server.metrics, RAFT_METRIC_CONNECTIONS_ACCEPTED);
        /* Preserve detailed connection logging for passive --serve demos.
         * Heartbeats create many short-lived connections in --elect mode;
         * logging each accepted socket would flood stdout and skew timing. */
        if (!elections) {
            char address_text[INET_ADDRSTRLEN] = "<unknown>";
            (void)inet_ntop(AF_INET, &remote.sin_addr,
                            address_text, sizeof(address_text));
            printf("Accepted: node=%u connection=%u from=%s:%u\n",
                   config->node_id, accepted, address_text,
                   (unsigned)ntohs(remote.sin_port));
            (void)fflush(stdout);
        }

        tcp_worker_t *worker = free_worker_slot(&server);
        /* free_worker_slot() is valid: only the accept thread sets in_use. */
        worker->client_fd = client;
        worker->server = &server;
        worker->finished = false;
        worker->in_use = true;
        raft_metrics_connection_open(&server.metrics);
        code = pthread_create(&worker->thread, NULL, tcp_worker_main, worker);
        if (code != 0) {
            raft_metrics_connection_close(&server.metrics);
            worker->in_use = false;
            (void)close(client);
            report_error(error, error_capacity, "pthread_create: %s", strerror(code));
            result = -1;
            break;
        }
    }

    (void)close(listener);
    /* On a signal or fatal error abort stalled clients; at max_connections,
     * drain all accepted requests before exiting normally. */
    stop_and_join_workers(&server, stop_requested != 0 || result != 0);
    if (server.storage_failed) {
        report_error(error, error_capacity, "fatal durable storage failure; node stopped");
        result = -1;
    }
    if (hup_installed) (void)sigaction(SIGHUP, &previous_hup, NULL);
    (void)sigaction(SIGTERM, &previous_term, NULL);
    (void)sigaction(SIGINT, &previous_int, NULL);
    if (result == 0) {
        printf("Stopped: node=%u accepted=%u\n", config->node_id, accepted);
        (void)fflush(stdout);
        if (error != NULL && error_capacity != 0) error[0] = '\0';
    }
    return result;
}

int raft_tcp_serve(const raft_cluster_config_t *config,
                   unsigned int max_connections,
                   char *error, size_t error_capacity)
{
    return tcp_serve_mode(config, max_connections, false, NULL, false, false, false, NULL, error, error_capacity);
}

int raft_tcp_elect(const raft_cluster_config_t *config,
                  char *error, size_t error_capacity)
{
    return tcp_serve_mode(config, 0, true, NULL, false, false, false, NULL, error, error_capacity);
}

int raft_tcp_elect_durable(const raft_cluster_config_t *config,
                           const char *directory, char *error, size_t capacity)
{
    if (!directory || !*directory) {
        report_error(error, capacity, "durable directory is required");
        return -1;
    }
    return tcp_serve_mode(config, 0, true, directory, false, false, false, NULL, error, capacity);
}

int raft_tcp_elect_wal(const raft_cluster_config_t *config,
                       const char *directory, char *error, size_t capacity)
{
    if (!directory || !*directory) {
        report_error(error,capacity,"WAL directory is required");
        return -1;
    }
    return tcp_serve_mode(config,0,true,directory,true,false,false,NULL,error,capacity);
}

int raft_tcp_elect_snapshot(const raft_cluster_config_t *config,
                            char *error, size_t capacity)
{
    return tcp_serve_mode(config,0,true,NULL,false,true,false,NULL,error,capacity);
}

int raft_tcp_elect_snapshot_wal(const raft_cluster_config_t *config,
                               const char *directory, char *error, size_t capacity)
{
    if (!directory || !*directory) {
        report_error(error, capacity, "snapshot WAL directory is required");
        return -1;
    }
    return tcp_serve_mode(config,0,true,directory,false,true,true,NULL,error,capacity);
}

int raft_tcp_elect_auth(const raft_cluster_config_t *config,
                       const char *keyring, char *error, size_t capacity)
{
    if (!keyring || !*keyring) return -1;
    return tcp_serve_mode(config,0,true,NULL,false,false,false,keyring,error,capacity);
}

int raft_tcp_elect_auth_wal(const raft_cluster_config_t *config,
                           const char *directory, const char *keyring,
                           char *error, size_t capacity)
{
    if (!directory || !*directory || !keyring || !*keyring) return -1;
    return tcp_serve_mode(config,0,true,directory,false,true,true,keyring,error,capacity);
}
