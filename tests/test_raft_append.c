#include "raft_append.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

static unsigned checks;
#define CHECK(x) do { assert(x); ++checks; } while (0)

static raft_node_t follower(void)
{
    raft_node_t n;
    CHECK(raft_node_init(&n, 2) == RAFT_OK);
    CHECK(raft_node_start_follower(&n, 100, 72) == RAFT_OK);
    return n;
}

static void test_wire(void)
{
    raft_append_request_t req = {0x0102030405060708ULL, 3, 9, 5, 4};
    uint8_t bytes[RAFT_APPEND_REQUEST_BYTES];
    CHECK(raft_append_encode_request(&req, bytes) == RAFT_OK);
    const uint8_t term[] = {1,2,3,4,5,6,7,8};
    CHECK(memcmp(bytes, term, 8) == 0);
    CHECK(bytes[8] == 0 && bytes[11] == 3);
    CHECK(bytes[19] == 9 && bytes[27] == 5 && bytes[35] == 4);
    raft_append_request_t decoded = {99, 99, 99, 99, 99};
    CHECK(raft_append_decode_request(bytes, sizeof(bytes), &decoded) == RAFT_OK);
    CHECK(decoded.term == req.term && decoded.leader_id == req.leader_id &&
          decoded.prev_log_index == 9 && decoded.prev_log_term == 5 &&
          decoded.leader_commit == 4);
    CHECK(raft_append_decode_request(bytes, 35, &decoded) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(decoded.term == req.term);
    bytes[11] = 0; /* nonmember 0 invalid */
    CHECK(raft_append_decode_request(bytes, 36, &decoded) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(decoded.leader_id == 3);
    bytes[11] = 3;
    memset(bytes+12, 0, 8);
    CHECK(raft_append_decode_request(bytes, 36, &decoded) == RAFT_ERR_INVALID_ARGUMENT);
    raft_append_response_t rsp = {0x0102030405060708ULL, 1};
    uint8_t rb[RAFT_APPEND_RESPONSE_BYTES];
    CHECK(raft_append_encode_response(&rsp, rb) == RAFT_OK);
    CHECK(memcmp(rb, term, 8) == 0 && rb[8] == 1);
    raft_append_response_t output = {42, 0};
    CHECK(raft_append_decode_response(rb, 9, &output) == RAFT_OK);
    CHECK(output.term == rsp.term && output.success == 1);
    rb[8] = 2;
    CHECK(raft_append_decode_response(rb, 9, &output) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(output.term == rsp.term && output.success == 1);
    CHECK(raft_append_decode_response(rb, 8, &output) == RAFT_ERR_INVALID_ARGUMENT);
    rsp.success = -1;
    CHECK(raft_append_encode_response(&rsp, rb) == RAFT_ERR_INVALID_ARGUMENT);
}

static void test_term_timer_and_roles(void)
{
    raft_node_t n = follower();
    raft_append_request_t req = {3, 1, 0, 0, 0};
    raft_append_response_t rsp = {0};
    uint64_t old_deadline = n.election_timer.deadline_ms;
    CHECK(raft_append_handle(&n, &req, 180, &rsp) == RAFT_OK);
    CHECK(n.role == RAFT_FOLLOWER && n.current_term == 3 &&
          n.voted_for == 0 && rsp.term == 3 && rsp.success == 1);
    CHECK(n.election_timer.deadline_ms >= 180 + RAFT_ELECTION_TIMEOUT_MIN_MS &&
          n.election_timer.deadline_ms <= 180 + RAFT_ELECTION_TIMEOUT_MAX_MS);
    old_deadline = n.election_timer.deadline_ms;
    req.term = 2;
    CHECK(raft_append_handle(&n, &req, 181, &rsp) == RAFT_OK);
    CHECK(rsp.term == 3 && rsp.success == 0 && n.election_timer.deadline_ms == old_deadline);
    req.term = 3;
    n.role = RAFT_CANDIDATE;
    n.voted_for = 2;
    CHECK(raft_append_handle(&n, &req, 182, &rsp) == RAFT_OK);
    CHECK(rsp.success == 1 && n.role == RAFT_FOLLOWER && n.voted_for == 2);
    n.role = RAFT_LEADER;
    CHECK(raft_append_handle(&n, &req, 183, &rsp) == RAFT_OK);
    CHECK(n.role == RAFT_FOLLOWER && n.current_term == 3);
    req.leader_id = 2;
    CHECK(raft_append_handle(&n, &req, 184, &rsp) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(n.role == RAFT_FOLLOWER);
    raft_node_destroy(&n);
}

static void test_log_consistency_commit(void)
{
    raft_node_t n = follower();
    n.current_term = 5;
    CHECK(raft_log_append(&n.log, 3, "x", 1) == RAFT_OK);
    CHECK(raft_log_append(&n.log, 5, "y", 1) == RAFT_OK);
    raft_append_request_t req = {5, 1, 2, 4, 2};
    raft_append_response_t rsp;
    CHECK(raft_append_handle(&n, &req, 250, &rsp) == RAFT_OK);
    CHECK(rsp.success == 0 && n.commit_index == 0);
    CHECK(n.election_timer.deadline_ms >= 250 + RAFT_ELECTION_TIMEOUT_MIN_MS &&
          n.election_timer.deadline_ms <= 250 + RAFT_ELECTION_TIMEOUT_MAX_MS); /* mismatch still resets */
    req.prev_log_term = 5;
    CHECK(raft_append_handle(&n, &req, 260, &rsp) == RAFT_OK);
    CHECK(rsp.success == 1 && n.commit_index == 2);
    req.leader_commit = UINT64_MAX;
    CHECK(raft_append_handle(&n, &req, 270, &rsp) == RAFT_OK);
    CHECK(n.commit_index == 2); /* cannot commit beyond proven matching index */
    req.prev_log_index = 3;
    CHECK(raft_append_handle(&n, &req, 280, &rsp) == RAFT_OK);
    CHECK(rsp.success == 0 && n.commit_index == 2);
    raft_node_destroy(&n);
}

static void test_higher_term_response(void)
{
    raft_node_t n = follower();
    n.current_term = 4;
    n.role = RAFT_LEADER;
    n.voted_for = 2;
    raft_append_response_t r = {4, 0};
    CHECK(raft_append_handle_response(&n, &r, 201) == RAFT_OK);
    CHECK(n.role == RAFT_LEADER);
    r.term = 6;
    CHECK(raft_append_handle_response(&n, &r, 202) == RAFT_OK);
    CHECK(n.current_term == 6 && n.role == RAFT_FOLLOWER && n.voted_for == 0);
    CHECK(n.election_timer.deadline_ms > 202);
    r.term = 3;
    CHECK(raft_append_handle_response(&n, &r, 203) == RAFT_OK);
    CHECK(n.current_term == 6);
    r.success = 4;
    CHECK(raft_append_handle_response(&n, &r, 204) == RAFT_ERR_INVALID_ARGUMENT);
    raft_node_destroy(&n);
}

int main(void)
{
    test_wire();
    test_term_timer_and_roles();
    test_log_consistency_commit();
    test_higher_term_response();
    printf("PASS: %u AppendEntries encoding, term, timer, role, consistency assertions\n", checks);
    return 0;
}
