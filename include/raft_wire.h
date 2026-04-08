#ifndef RAFT_WIRE_H
#define RAFT_WIRE_H

#include <stddef.h>
#include <stdint.h>

/* Length-prefixed transport shared by PING/PONG and RequestVote.
 * 12-byte header: magic:u32, version:u8, type:u8, reserved:u16, length:u32.
 * All multi-byte integers use network (big-endian) byte order.
 */
#define RAFT_WIRE_HEADER_SIZE 12u
#define RAFT_WIRE_MAGIC UINT32_C(0x52414654) /* ASCII RAFT */
#define RAFT_WIRE_VERSION 1u
#define RAFT_WIRE_MAX_PAYLOAD 4096u

/* Frame types. All RequestVote payloads use raft_vote.h encoding. */
typedef enum {
    RAFT_WIRE_PING = 1,
    RAFT_WIRE_PONG = 2,
    RAFT_WIRE_REQUEST_VOTE = 3,
    RAFT_WIRE_REQUEST_VOTE_RESPONSE = 4
} raft_wire_type_t;

typedef enum {
    RAFT_WIRE_OK = 0,
    RAFT_WIRE_CLOSED = 1,    /* clean EOF before any next-frame header bytes */
    RAFT_WIRE_ARGUMENT = -1,
    RAFT_WIRE_PROTOCOL = -2,
    RAFT_WIRE_IO = -3
} raft_wire_status_t;

typedef struct {
    raft_wire_type_t type;
    uint32_t length;
} raft_wire_header_t;

const char *raft_wire_status_name(raft_wire_status_t status);

/* Pure serializer: no I/O; out is exactly RAFT_WIRE_HEADER_SIZE bytes. */
raft_wire_status_t raft_wire_encode_header(raft_wire_header_t header,
                                           uint8_t out[RAFT_WIRE_HEADER_SIZE]);
raft_wire_status_t raft_wire_decode_header(const uint8_t data[RAFT_WIRE_HEADER_SIZE],
                                           raft_wire_header_t *out);

/* Write exactly one frame. Does not close or own fd.
 * Requires a connected socket, optionally with a SO_SNDTIMEO.
 * SIGPIPE is suppressed on Linux via MSG_NOSIGNAL.
 */
raft_wire_status_t raft_wire_send(int fd, raft_wire_type_t type,
                                  const void *payload, size_t length,
                                  char *error, size_t capacity);

/* Read exactly one frame; payload is binary, not a C string.
 * On success sets type/length and copies length bytes to caller buffer.
 * On failure leaves type/length/buffer unchanged.
 * Caller owns fd, and should close a connection after framing failure.
 */
raft_wire_status_t raft_wire_recv(int fd, raft_wire_type_t *type,
                                  void *buffer, size_t buffer_capacity,
                                  size_t *length,
                                  char *error, size_t capacity);

#endif /* RAFT_WIRE_H */
