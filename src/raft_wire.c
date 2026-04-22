#define _POSIX_C_SOURCE 200809L

#include "raft_wire.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

static void report_error(char *dst, size_t capacity, const char *fmt, ...)
{
    if (dst == NULL || capacity == 0) return;
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(dst, capacity, fmt, args);
    va_end(args);
}

static void clear_error(char *dst, size_t capacity)
{
    if (dst != NULL && capacity != 0) dst[0] = '\0';
}

static void store_u32(uint8_t *dst, uint32_t n)
{
    dst[0] = (uint8_t)(n >> 24);
    dst[1] = (uint8_t)(n >> 16);
    dst[2] = (uint8_t)(n >> 8);
    dst[3] = (uint8_t)n;
}

static uint32_t load_u32(const uint8_t *src)
{
    return ((uint32_t)src[0] << 24) |
           ((uint32_t)src[1] << 16) |
           ((uint32_t)src[2] << 8) |
           (uint32_t)src[3];
}

static int valid_type(raft_wire_type_t type)
{
    return type == RAFT_WIRE_PING || type == RAFT_WIRE_PONG ||
           type == RAFT_WIRE_REQUEST_VOTE ||
           type == RAFT_WIRE_REQUEST_VOTE_RESPONSE ||
           type == RAFT_WIRE_STATUS_REQUEST ||
           type == RAFT_WIRE_STATUS_RESPONSE ||
           type == RAFT_WIRE_APPEND_ENTRIES ||
           type == RAFT_WIRE_APPEND_ENTRIES_RESPONSE ||
           type == RAFT_WIRE_LOG_STATUS_REQUEST ||
           type == RAFT_WIRE_LOG_STATUS_RESPONSE ||
           type == RAFT_WIRE_DEMO_PROPOSE ||
           type == RAFT_WIRE_DEMO_PROPOSE_RESPONSE ||
           type == RAFT_WIRE_APPLY_STATUS_REQUEST ||
           type == RAFT_WIRE_APPLY_STATUS_RESPONSE ||
           type == RAFT_WIRE_DEMO_GET_REQUEST ||
           type == RAFT_WIRE_DEMO_GET_RESPONSE ||
           type == RAFT_WIRE_INSTALL_SNAPSHOT ||
           type == RAFT_WIRE_INSTALL_SNAPSHOT_RESPONSE ||
           type == RAFT_WIRE_SNAPSHOT_COMPACT_DEMO ||
           type == RAFT_WIRE_SNAPSHOT_COMPACT_RESPONSE ||
           type == RAFT_WIRE_CLIENT_WRITE ||
           type == RAFT_WIRE_CLIENT_WRITE_RESPONSE;
}

const char *raft_wire_status_name(raft_wire_status_t status)
{
    switch (status) {
    case RAFT_WIRE_OK: return "ok";
    case RAFT_WIRE_CLOSED: return "closed";
    case RAFT_WIRE_ARGUMENT: return "argument";
    case RAFT_WIRE_PROTOCOL: return "protocol";
    case RAFT_WIRE_IO: return "io";
    default: return "unknown";
    }
}

raft_wire_status_t raft_wire_encode_header(raft_wire_header_t header,
                                           uint8_t out[RAFT_WIRE_HEADER_SIZE])
{
    if (out == NULL || !valid_type(header.type) ||
        header.length > RAFT_WIRE_MAX_PAYLOAD)
        return RAFT_WIRE_ARGUMENT;
    store_u32(out, RAFT_WIRE_MAGIC);
    out[4] = RAFT_WIRE_VERSION;
    out[5] = (uint8_t)header.type;
    out[6] = 0;
    out[7] = 0;
    store_u32(out + 8, header.length);
    return RAFT_WIRE_OK;
}

raft_wire_status_t raft_wire_decode_header(const uint8_t data[RAFT_WIRE_HEADER_SIZE],
                                           raft_wire_header_t *out)
{
    if (data == NULL || out == NULL) return RAFT_WIRE_ARGUMENT;
    if (load_u32(data) != RAFT_WIRE_MAGIC ||
        data[4] != RAFT_WIRE_VERSION ||
        !valid_type((raft_wire_type_t)data[5]) ||
        data[6] != 0 || data[7] != 0 ||
        load_u32(data + 8) > RAFT_WIRE_MAX_PAYLOAD)
        return RAFT_WIRE_PROTOCOL;
    raft_wire_header_t decoded = {
        .type = (raft_wire_type_t)data[5],
        .length = load_u32(data + 8)
    };
    *out = decoded;
    return RAFT_WIRE_OK;
}

/* Treat EOF before any header bytes as a clean close. EOF mid-frame is invalid. */
static raft_wire_status_t read_exact(int fd, uint8_t *buf, size_t n,
                                     int allow_clean_close,
                                     char *error, size_t capacity)
{
    size_t done = 0;
    while (done < n) {
        ssize_t got = recv(fd, buf + done, n - done, 0);
        if (got > 0) {
            done += (size_t)got;
        } else if (got == 0) {
            if (allow_clean_close && done == 0) return RAFT_WIRE_CLOSED;
            report_error(error, capacity, "incomplete frame: peer closed after %zu/%zu bytes",
                         done, n);
            return RAFT_WIRE_PROTOCOL;
        } else if (errno != EINTR) {
            report_error(error, capacity, "recv: %s", strerror(errno));
            return RAFT_WIRE_IO;
        }
    }
    return RAFT_WIRE_OK;
}

static raft_wire_status_t write_exact(int fd, const uint8_t *buf, size_t n,
                                      char *error, size_t capacity)
{
    size_t done = 0;
    while (done < n) {
        ssize_t sent = send(fd, buf + done, n - done, MSG_NOSIGNAL);
        if (sent > 0) {
            done += (size_t)sent;
        } else if (sent == 0) {
            report_error(error, capacity, "send: zero bytes written");
            return RAFT_WIRE_IO;
        } else if (errno != EINTR) {
            report_error(error, capacity, "send: %s", strerror(errno));
            return RAFT_WIRE_IO;
        }
    }
    return RAFT_WIRE_OK;
}

raft_wire_status_t raft_wire_send(int fd, raft_wire_type_t type,
                                  const void *payload, size_t length,
                                  char *error, size_t capacity)
{
    if (fd < 0 || length > RAFT_WIRE_MAX_PAYLOAD ||
        (length != 0 && payload == NULL)) {
        report_error(error, capacity, "invalid send arguments");
        return RAFT_WIRE_ARGUMENT;
    }
    uint8_t header[RAFT_WIRE_HEADER_SIZE];
    if (raft_wire_encode_header((raft_wire_header_t){type, (uint32_t)length},
                                header) != RAFT_WIRE_OK) {
        report_error(error, capacity, "invalid message type or length");
        return RAFT_WIRE_ARGUMENT;
    }
    raft_wire_status_t status = write_exact(fd, header, sizeof(header), error, capacity);
    if (status != RAFT_WIRE_OK) return status;
    if (length != 0) {
        status = write_exact(fd, payload, length, error, capacity);
        if (status != RAFT_WIRE_OK) return status;
    }
    clear_error(error, capacity);
    return RAFT_WIRE_OK;
}

raft_wire_status_t raft_wire_recv(int fd, raft_wire_type_t *type,
                                  void *buffer, size_t buffer_capacity,
                                  size_t *length, char *error, size_t capacity)
{
    if (fd < 0 || type == NULL || length == NULL ||
        (buffer_capacity != 0 && buffer == NULL)) {
        report_error(error, capacity, "invalid receive arguments");
        return RAFT_WIRE_ARGUMENT;
    }
    uint8_t raw[RAFT_WIRE_HEADER_SIZE];
    raft_wire_status_t status = read_exact(fd, raw, sizeof(raw), 1, error, capacity);
    if (status != RAFT_WIRE_OK) {
        if (status == RAFT_WIRE_CLOSED) clear_error(error, capacity);
        return status;
    }
    raft_wire_header_t header;
    status = raft_wire_decode_header(raw, &header);
    if (status != RAFT_WIRE_OK) {
        report_error(error, capacity, "invalid frame header (magic/version/type/flags/length)");
        return status;
    }
    if (header.length > buffer_capacity) {
        report_error(error, capacity, "payload length %u exceeds destination capacity %zu",
                     (unsigned)header.length, buffer_capacity);
        return RAFT_WIRE_ARGUMENT;
    }
    uint8_t temporary[RAFT_WIRE_MAX_PAYLOAD];
    if (header.length != 0) {
        status = read_exact(fd, temporary, header.length, 0, error, capacity);
        if (status != RAFT_WIRE_OK) return status;
        memcpy(buffer, temporary, header.length);
    }
    *type = header.type;
    *length = header.length;
    clear_error(error, capacity);
    return RAFT_WIRE_OK;
}
