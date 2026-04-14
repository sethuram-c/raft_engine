#define _POSIX_C_SOURCE 200809L

#include "raft_tcp.h"
#include "raft_wire.h"
#include "raft_vote.h"
#include "raft_election.h"
#include "raft_append.h"
#include "raft_replication.h"
#include "raft_commit.h"

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
    raft_node_t vote_node; /* Protected by server->mutex. Memory-only Phase 8. */
    raft_cluster_config_t config; /* Fixed set of allowed candidate IDs. */
    pthread_t election_thread;
    bool election_thread_started;
    atomic_bool election_stop;
    raft_replication_progress_t progress[RAFT_ELECTION_CLUSTER_SIZE];
};

static void process_client(int fd, tcp_server_t *server)
{
    uint8_t payload[RAFT_WIRE_MAX_PAYLOAD];
    raft_wire_type_t type;
    size_t length = 0;
    char message[256] = {0};
    raft_wire_status_t status = raft_wire_recv(fd, &type, payload,
                                   sizeof(payload), &length, message, sizeof(message));
    if (status == RAFT_WIRE_OK && type == RAFT_WIRE_PING) {
        status = raft_wire_send(fd, RAFT_WIRE_PONG, payload, length,
                                message, sizeof(message));
        if (status == RAFT_WIRE_OK) {
            printf("Replied: node=%u bytes=%zu\n", server->node_id, length);
            (void)fflush(stdout);
        }
    } else if (status == RAFT_WIRE_OK && type == RAFT_WIRE_REQUEST_VOTE) {
        raft_vote_request_t req;
        raft_vote_response_t rsp;
        uint8_t output[RAFT_VOTE_RESPONSE_BYTES];
        uint64_t now_ms = 0;
        if (raft_vote_decode_request(payload, length, &req) != RAFT_OK ||
            raft_config_peer_for_id(&server->config, req.candidate_id) == NULL ||
            req.candidate_id == server->node_id ||
            raft_clock_monotonic_ms(&now_ms) != RAFT_OK) {
            fputs("Rejected malformed RequestVote payload\n", stderr);
            return;
        }
        (void)pthread_mutex_lock(&server->mutex);
        raft_status_t result = raft_vote_handle(&server->vote_node, &req, now_ms, &rsp);
        (void)pthread_mutex_unlock(&server->mutex);
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
            raft_clock_monotonic_ms(&now_ms) != RAFT_OK) {
            fputs("Rejected malformed or nonmember AppendEntries\n", stderr);
            return;
        }
        (void)pthread_mutex_lock(&server->mutex);
        raft_status_t result = raft_append_handle_batch(&server->vote_node, &batch, now_ms, &rsp);
        raft_index_t last = raft_log_last_index(&server->vote_node.log);
        (void)pthread_mutex_unlock(&server->mutex);
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
    } else if (status == RAFT_WIRE_OK && type == RAFT_WIRE_DEMO_PROPOSE &&
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
        fputs("Rejected frame: expected PING, RequestVote, AppendEntries or empty STATUS request\n", stderr);
    }
    if (status != RAFT_WIRE_OK && status != RAFT_WIRE_CLOSED)
        fprintf(stderr, "Rejected frame: %s\n", message);
}

static void *tcp_worker_main(void *arg)
{
    tcp_worker_t *worker = arg;
    tcp_server_t *server = worker->server;
    int fd = worker->client_fd;
    if (configure_client_timeouts(fd) == 0)
        process_client(fd, server);
    else
        fprintf(stderr, "Client socket timeout setup failed: %s\n", strerror(errno));

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
    atomic_init(&server->election_stop, false);
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
            begin = true;
            printf("Election started: node=%u term=%llu self_votes=1\n",
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
                (void)pthread_mutex_lock(&server->mutex);
                bool valid = server->vote_node.role == RAFT_LEADER &&
                    server->vote_node.current_term == heartbeat_term &&
                    server->progress[i].term == heartbeat_term &&
                    server->progress[i].peer_id == peer->id &&
                    raft_replication_prepare(&server->vote_node,
                        &server->progress[i], &batch, &sent_through) == RAFT_OK &&
                    raft_append_encode_batch(&batch, payload, sizeof(payload),
                                             &payload_length) == RAFT_OK;
                size_t entries = valid ? batch.entry_count : 0;
                (void)pthread_mutex_unlock(&server->mutex);
                if (!valid) break;
                raft_append_response_t reply;
                if (send_peer_replication(peer, payload, payload_length, &reply) != 0)
                    continue; /* network failure retains nextIndex for retry */
                if (raft_clock_monotonic_ms(&now) != RAFT_OK) break;
                (void)pthread_mutex_lock(&server->mutex);
                raft_role_t old = server->vote_node.role;
                (void)raft_append_handle_response(&server->vote_node, &reply, now);
                bool still_leader = server->vote_node.role == RAFT_LEADER &&
                    server->vote_node.current_term == heartbeat_term;
                if (still_leader) {
                    (void)raft_replication_observe(&server->progress[i],
                                                    sent_through, &reply);
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
                (void)raft_election_record(&server->vote_node, &round,
                                            peer->id, &reply, now);
                if (previous == RAFT_CANDIDATE &&
                    server->vote_node.role == RAFT_LEADER) {
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
                    next_heartbeat_ms = 0; /* send first AppendEntries next loop */
                }
                bool continuing = server->vote_node.role == RAFT_CANDIDATE &&
                                  server->vote_node.current_term == round.term;
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
                          char *error, size_t error_capacity)
{
    if (raft_config_validate(config, error, error_capacity) != RAFT_CONFIG_OK)
        return -1;
    const raft_peer_config_t *local =
        raft_config_peer_for_id(config, config->node_id);
    int listener = raft_tcp_listen(local, error, error_capacity);
    if (listener < 0) return -1;

    tcp_server_t server;
    int code = tcp_server_init(&server, config->node_id);
    if (code == 0) server.config = *config;
    if (code != 0) {
        report_error(error, error_capacity, "pthread server initialization: %s", strerror(code));
        (void)close(listener);
        return -1;
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

    if (elections) {
        code = pthread_create(&server.election_thread, NULL,
                              election_worker_main, &server);
        if (code != 0) {
            report_error(error, error_capacity, "election worker: %s", strerror(code));
            (void)close(listener);
            stop_and_join_workers(&server, false);
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
        code = pthread_create(&worker->thread, NULL, tcp_worker_main, worker);
        if (code != 0) {
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
    return tcp_serve_mode(config, max_connections, false, error, error_capacity);
}

int raft_tcp_elect(const raft_cluster_config_t *config,
                  char *error, size_t error_capacity)
{
    return tcp_serve_mode(config, 0, true, error, error_capacity);
}
