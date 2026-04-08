#define _POSIX_C_SOURCE 200809L

#include "raft_tcp.h"
#include "raft_wire.h"
#include "raft_vote.h"

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
    } else if (status == RAFT_WIRE_OK) {
        fputs("Rejected frame: expected PING or RequestVote request\n", stderr);
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
                                 0x9E3779B9U ^ node_id) != RAFT_OK) {
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

int raft_tcp_serve(const raft_cluster_config_t *config,
                   unsigned int max_connections,
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
        char address_text[INET_ADDRSTRLEN] = "<unknown>";
        (void)inet_ntop(AF_INET, &remote.sin_addr,
                        address_text, sizeof(address_text));
        printf("Accepted: node=%u connection=%u from=%s:%u\n",
               config->node_id, accepted, address_text,
               (unsigned)ntohs(remote.sin_port));
        (void)fflush(stdout);

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
