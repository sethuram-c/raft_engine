#define _POSIX_C_SOURCE 200809L

#include "raft_wire.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static unsigned int assertions = 0;
#define CHECK(expr) do { assert(expr); ++assertions; } while (0)

static void check_header_bytes(void)
{
    uint8_t data[RAFT_WIRE_HEADER_SIZE];
    raft_wire_header_t header = {.type = RAFT_WIRE_PING, .length = 0x0102u};
    CHECK(raft_wire_encode_header(header, data) == RAFT_WIRE_OK);
    const uint8_t expected[RAFT_WIRE_HEADER_SIZE] = {
        0x52, 0x41, 0x46, 0x54, 1, 1, 0, 0, 0, 0, 0x01, 0x02
    };
    CHECK(memcmp(data, expected, sizeof(data)) == 0);
    raft_wire_header_t decoded = {0};
    CHECK(raft_wire_decode_header(data, &decoded) == RAFT_WIRE_OK);
    CHECK(decoded.type == RAFT_WIRE_PING && decoded.length == 0x0102u);
    data[0] = 0;
    CHECK(raft_wire_decode_header(data, &decoded) == RAFT_WIRE_PROTOCOL);
    data[0] = 0x52;
    data[4] = 2;
    CHECK(raft_wire_decode_header(data, &decoded) == RAFT_WIRE_PROTOCOL);
    data[4] = 1;
    data[5] = 200;
    CHECK(raft_wire_decode_header(data, &decoded) == RAFT_WIRE_PROTOCOL);
    data[5] = 1;
    data[6] = 1;
    CHECK(raft_wire_decode_header(data, &decoded) == RAFT_WIRE_PROTOCOL);
    data[6] = 0;
    data[8] = 0;
    data[9] = 0;
    data[10] = 0x10;
    data[11] = 0x01;
    CHECK(raft_wire_decode_header(data, &decoded) == RAFT_WIRE_PROTOCOL);
    CHECK(raft_wire_encode_header((raft_wire_header_t){.type=0, .length=0}, data)
          == RAFT_WIRE_ARGUMENT);
    CHECK(raft_wire_encode_header((raft_wire_header_t){.type=RAFT_WIRE_PING,
          .length=RAFT_WIRE_MAX_PAYLOAD+1u}, data) == RAFT_WIRE_ARGUMENT);
}

static void round_trip_and_multiple_frames(void)
{
    int fds[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    const uint8_t bytes[] = {0, 4, 255, 'Z'};
    char error[128];
    uint8_t dest[RAFT_WIRE_MAX_PAYLOAD] = {0};
    size_t length = 999;
    raft_wire_type_t type = RAFT_WIRE_PONG;
    CHECK(raft_wire_send(fds[0], RAFT_WIRE_PING, bytes, sizeof(bytes),
                         error, sizeof(error)) == RAFT_WIRE_OK);
    CHECK(raft_wire_recv(fds[1], &type, dest, sizeof(dest), &length,
                         error, sizeof(error)) == RAFT_WIRE_OK);
    CHECK(type == RAFT_WIRE_PING && length == sizeof(bytes));
    CHECK(memcmp(bytes, dest, length) == 0);
    CHECK(raft_wire_send(fds[0], RAFT_WIRE_PONG, NULL, 0,
                         error, sizeof(error)) == RAFT_WIRE_OK);
    CHECK(raft_wire_recv(fds[1], &type, dest, sizeof(dest), &length,
                         error, sizeof(error)) == RAFT_WIRE_OK);
    CHECK(type == RAFT_WIRE_PONG && length == 0);
    CHECK(close(fds[0]) == 0);
    CHECK(raft_wire_recv(fds[1], &type, dest, sizeof(dest), &length,
                         error, sizeof(error)) == RAFT_WIRE_CLOSED);
    CHECK(close(fds[1]) == 0);
}

static void test_truncation_and_output_integrity(void)
{
    int fds[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    const uint8_t partial[4] = {0x52, 0x41, 0x46, 0x54};
    CHECK(write(fds[0], partial, sizeof(partial)) == (ssize_t)sizeof(partial));
    CHECK(close(fds[0]) == 0);
    uint8_t dest[16] = {99};
    raft_wire_type_t type = RAFT_WIRE_PONG;
    size_t length = 88;
    char error[128];
    CHECK(raft_wire_recv(fds[1], &type, dest, sizeof(dest), &length,
                         error, sizeof(error)) == RAFT_WIRE_PROTOCOL);
    CHECK(type == RAFT_WIRE_PONG && length == 88 && dest[0] == 99);
    CHECK(strstr(error, "incomplete") != NULL);
    CHECK(close(fds[1]) == 0);

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    uint8_t hdr[RAFT_WIRE_HEADER_SIZE];
    CHECK(raft_wire_encode_header((raft_wire_header_t){RAFT_WIRE_PING, 4}, hdr)
          == RAFT_WIRE_OK);
    CHECK(write(fds[0], hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr));
    CHECK(write(fds[0], "xy", 2) == 2);
    CHECK(close(fds[0]) == 0);
    CHECK(raft_wire_recv(fds[1], &type, dest, sizeof(dest), &length,
                         error, sizeof(error)) == RAFT_WIRE_PROTOCOL);
    CHECK(type == RAFT_WIRE_PONG && length == 88 && dest[0] == 99);
    CHECK(close(fds[1]) == 0);
}

static void test_capacity_and_argument_errors(void)
{
    int fds[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    char error[128];
    CHECK(raft_wire_send(fds[0], RAFT_WIRE_PING, NULL, 1,
                         error, sizeof(error)) == RAFT_WIRE_ARGUMENT);
    CHECK(raft_wire_send(fds[0], RAFT_WIRE_PING, "x",
                         RAFT_WIRE_MAX_PAYLOAD + 1u, error, sizeof(error))
          == RAFT_WIRE_ARGUMENT);
    CHECK(raft_wire_send(fds[0], RAFT_WIRE_PING, "1234", 4,
                         error, sizeof(error)) == RAFT_WIRE_OK);
    uint8_t dest[3] = {8, 8, 8};
    raft_wire_type_t type = RAFT_WIRE_PONG;
    size_t length = 91;
    CHECK(raft_wire_recv(fds[1], &type, dest, sizeof(dest), &length,
                         error, sizeof(error)) == RAFT_WIRE_ARGUMENT);
    CHECK(type == RAFT_WIRE_PONG && length == 91 && dest[0] == 8);
    CHECK(strstr(error, "capacity") != NULL);
    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
}

static void test_max_size(void)
{
    int fds[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    uint8_t data[RAFT_WIRE_MAX_PAYLOAD], out[RAFT_WIRE_MAX_PAYLOAD];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)i;
    char error[128];
    CHECK(raft_wire_send(fds[0], RAFT_WIRE_PING, data, sizeof(data),
                         error, sizeof(error)) == RAFT_WIRE_OK);
    size_t length = 0;
    raft_wire_type_t type = RAFT_WIRE_PONG;
    CHECK(raft_wire_recv(fds[1], &type, out, sizeof(out), &length,
                         error, sizeof(error)) == RAFT_WIRE_OK);
    CHECK(type == RAFT_WIRE_PING && length == sizeof(data));
    CHECK(memcmp(data, out, sizeof(data)) == 0);
    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
}

int main(void)
{
    check_header_bytes();
    round_trip_and_multiple_frames();
    test_truncation_and_output_integrity();
    test_capacity_and_argument_errors();
    test_max_size();
    printf("PASS: %u wire protocol assertions\n", assertions);
    return 0;
}
