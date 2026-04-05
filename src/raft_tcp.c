#define _POSIX_C_SOURCE 200809L

#include "raft_tcp.h"
#include "raft_wire.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
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

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_stop_signal;
    (void)sigemptyset(&action.sa_mask);
    struct sigaction previous_int, previous_term;
    if (sigaction(SIGINT, &action, &previous_int) < 0) {
        report_error(error, error_capacity, "install SIGINT handler: %s", strerror(errno));
        (void)close(listener);
        return -1;
    }
    if (sigaction(SIGTERM, &action, &previous_term) < 0) {
        report_error(error, error_capacity, "install SIGTERM handler: %s", strerror(errno));
        (void)sigaction(SIGINT, &previous_int, NULL);
        (void)close(listener);
        return -1;
    }
    stop_requested = 0;
    unsigned int accepted = 0;
    int result = 0;
    printf("Listening: node=%u %s:%u (max_connections=%u)\n",
           config->node_id, local->ipv4, (unsigned)local->port,
           max_connections);
    (void)fflush(stdout);

    while (!stop_requested &&
           (max_connections == 0 || accepted < max_connections)) {
        struct pollfd ready = {.fd = listener, .events = POLLIN, .revents = 0};
        int n = poll(&ready, 1, 250);
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
        if (configure_client_timeouts(client) != 0) {
            fprintf(stderr, "Client socket timeout setup failed: %s\n", strerror(errno));
            (void)close(client);
            continue;
        }
        uint8_t payload[RAFT_WIRE_MAX_PAYLOAD];
        raft_wire_type_t type;
        size_t payload_length = 0;
        char frame_error[256] = {0};
        raft_wire_status_t frame_status =
            raft_wire_recv(client, &type, payload, sizeof(payload),
                           &payload_length, frame_error, sizeof(frame_error));
        if (frame_status == RAFT_WIRE_OK && type == RAFT_WIRE_PING) {
            frame_status = raft_wire_send(client, RAFT_WIRE_PONG,
                                          payload, payload_length,
                                          frame_error, sizeof(frame_error));
            if (frame_status == RAFT_WIRE_OK) {
                printf("Replied: node=%u bytes=%zu\n", config->node_id, payload_length);
                (void)fflush(stdout);
            }
        } else if (frame_status == RAFT_WIRE_OK) {
            fprintf(stderr, "Rejected frame: expected PING request\n");
        }
        if (frame_status != RAFT_WIRE_OK && frame_status != RAFT_WIRE_CLOSED)
            fprintf(stderr, "Rejected frame: %s\n", frame_error);
        (void)close(client);
    }

    (void)close(listener);
    (void)sigaction(SIGTERM, &previous_term, NULL);
    (void)sigaction(SIGINT, &previous_int, NULL);
    if (result == 0) {
        printf("Stopped: node=%u accepted=%u\n", config->node_id, accepted);
        (void)fflush(stdout);
        if (error != NULL && error_capacity != 0) error[0] = '\0';
    }
    return result;
}
