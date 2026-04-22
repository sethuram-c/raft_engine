#include "raft_client_write.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned assertions = 0;
#define CHECK(x) do { ++assertions; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

static int test_commands(void)
{
    CHECK(raft_client_write_valid_command((const uint8_t *)"SET a 1", 7));
    CHECK(raft_client_write_valid_command((const uint8_t *)"SET a ", 6));
    CHECK(raft_client_write_valid_command((const uint8_t *)"DEL z", 5));
    CHECK(!raft_client_write_valid_command(NULL, 3));
    CHECK(!raft_client_write_valid_command((const uint8_t *)"SET x", 5));
    CHECK(!raft_client_write_valid_command((const uint8_t *)"PUT x y", 7));
    CHECK(!raft_client_write_valid_command((const uint8_t *)"DEL ", 4));
    CHECK(!raft_client_write_valid_command((const uint8_t *)"DEL x z", 7));
    const uint8_t binary[] = {'S','E','T',' ','k',' ','A',0,255};
    CHECK(raft_client_write_valid_command(binary, sizeof(binary)));
    const uint8_t invalid_key[] = {'S','E','T',' ','\n',' ','v'};
    CHECK(!raft_client_write_valid_command(invalid_key, sizeof(invalid_key)));
    uint8_t huge[1025];
    memset(huge, 'A', sizeof(huge));
    CHECK(!raft_client_write_valid_command(huge, sizeof(huge)));
    return 0;
}

static int test_codec(void)
{
    const uint8_t cmd[] = {'S','E','T',' ','k',' ','a',0,255};
    uint8_t buf[1028];
    size_t n = 99;
    CHECK(raft_client_write_encode_request(750, cmd, sizeof(cmd), buf, sizeof(buf), &n) == 0);
    CHECK(n == 4 + sizeof(cmd));
    CHECK(buf[0] == 0 && buf[1] == 0 && buf[2] == 2 && buf[3] == 238);
    CHECK(memcmp(buf + 4, cmd, sizeof(cmd)) == 0);
    raft_client_write_request_t req = {0};
    CHECK(raft_client_write_decode_request(buf, n, &req) == 0);
    CHECK(req.timeout_ms == 750 && req.command_len == sizeof(cmd));
    CHECK(memcmp(req.command, cmd, sizeof(cmd)) == 0);
    CHECK(raft_client_write_encode_request(99, cmd, sizeof(cmd), buf, sizeof(buf), &n) != 0);
    CHECK(raft_client_write_encode_request(3001, cmd, sizeof(cmd), buf, sizeof(buf), &n) != 0);
    CHECK(raft_client_write_encode_request(750, cmd, sizeof(cmd), buf, 3, &n) != 0);
    CHECK(raft_client_write_decode_request(buf, 0, &req) != 0);
    CHECK(raft_client_write_decode_request(buf, 1029, &req) != 0);
    buf[2] = 0; buf[3] = 10;
    CHECK(raft_client_write_decode_request(buf, n, &req) != 0);
    CHECK(req.timeout_ms == 750); /* decoder does not partially mutate */

    raft_client_write_response_t rsp = {UINT64_C(0x0102030405060708), 42,
                                       RAFT_CLIENT_WRITE_COMMITTED};
    uint8_t out[RAFT_CLIENT_WRITE_RESPONSE_BYTES];
    CHECK(raft_client_write_encode_response(&rsp, out) == 0);
    CHECK(out[0] == 1 && out[7] == 8 && out[15] == 42 && out[16] == 0);
    raft_client_write_response_t parsed = {0};
    CHECK(raft_client_write_decode_response(out, sizeof(out), &parsed) == 0);
    CHECK(parsed.term == rsp.term && parsed.index == 42 &&
          parsed.status == RAFT_CLIENT_WRITE_COMMITTED);
    CHECK(raft_client_write_decode_response(out, sizeof(out) - 1, &parsed) != 0);
    out[16] = 5;
    CHECK(raft_client_write_decode_response(out, sizeof(out), &parsed) != 0);
    CHECK(parsed.index == 42);
    rsp.index = 0;
    CHECK(raft_client_write_encode_response(&rsp, out) != 0);
    rsp.status = RAFT_CLIENT_WRITE_TIMEOUT;
    CHECK(raft_client_write_encode_response(&rsp, out) == 0);
    CHECK(strcmp(raft_client_write_status_name(RAFT_CLIENT_WRITE_TIMEOUT), "TIMEOUT") == 0);
    return 0;
}

static int test_commit_rule(void)
{
    raft_node_t node;
    CHECK(raft_node_init(&node, 1) == RAFT_OK);
    node.current_term = 4;
    node.role = RAFT_LEADER;
    const uint8_t command[] = "SET k x";
    CHECK(raft_log_append(&node.log, 4, command, sizeof(command)-1) == RAFT_OK);
    CHECK(raft_client_write_check(&node, 4, 1) == RAFT_CLIENT_WRITE_PENDING);
    CHECK(raft_client_write_check(&node, 4, 0) == RAFT_CLIENT_WRITE_UNKNOWN);
    CHECK(raft_client_write_check(&node, 3, 1) == RAFT_CLIENT_WRITE_UNKNOWN);
    CHECK(raft_client_write_check(&node, 4, 2) == RAFT_CLIENT_WRITE_UNKNOWN);
    node.commit_index = 1;
    CHECK(raft_client_write_check(&node, 4, 1) == RAFT_CLIENT_WRITE_COMMITTED);
    node.role = RAFT_FOLLOWER;
    CHECK(raft_client_write_check(&node, 4, 1) == RAFT_CLIENT_WRITE_UNKNOWN);
    node.role = RAFT_LEADER;
    node.current_term = 5;
    CHECK(raft_client_write_check(&node, 4, 1) == RAFT_CLIENT_WRITE_UNKNOWN);
    raft_node_destroy(&node);
    return 0;
}

int main(void)
{
    if (test_commands() || test_codec() || test_commit_rule()) return 1;
    printf("PASS: %u Phase 22 write codec, validation, commitment assertions\n", assertions);
    return 0;
}
