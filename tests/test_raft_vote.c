#include "raft_vote.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { assert(x); ++checks; } while (0)

static void fresh(raft_node_t *n, raft_node_id_t id, raft_term_t term)
{
    CHECK(raft_node_init(n, id) == RAFT_OK);
    n->current_term = term;
    CHECK(raft_node_start_follower(n, 1000, 12345) == RAFT_OK);
}

static void test_wire(void)
{
    raft_vote_request_t r = {UINT64_C(0x0102030405060708), 0x10203040,
                             UINT64_C(0x1112131415161718), 5};
    uint8_t bytes[RAFT_VOTE_REQUEST_BYTES];
    CHECK(raft_vote_encode_request(&r, bytes) == RAFT_OK);
    const uint8_t expected[] = {1,2,3,4,5,6,7,8,0x10,0x20,0x30,0x40,
                                0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,
                                0,0,0,0,0,0,0,5};
    CHECK(memcmp(bytes, expected, sizeof(expected)) == 0);
    raft_vote_request_t unpack = {0};
    CHECK(raft_vote_decode_request(bytes, sizeof(bytes), &unpack) == RAFT_OK);
    CHECK(unpack.term == r.term && unpack.candidate_id == r.candidate_id &&
          unpack.last_log_index == r.last_log_index && unpack.last_log_term == r.last_log_term);
    raft_vote_request_t snapshot = unpack;
    CHECK(raft_vote_decode_request(bytes, sizeof(bytes)-1, &unpack) != RAFT_OK);
    CHECK(memcmp(&unpack, &snapshot, sizeof(unpack)) == 0);
    bytes[8]=bytes[9]=bytes[10]=bytes[11]=0;
    CHECK(raft_vote_decode_request(bytes, sizeof(bytes), &unpack) != RAFT_OK);
    CHECK(memcmp(&unpack, &snapshot, sizeof(unpack)) == 0);
    raft_vote_response_t response = {UINT64_MAX, 1};
    uint8_t result[RAFT_VOTE_RESPONSE_BYTES];
    CHECK(raft_vote_encode_response(&response, result) == RAFT_OK);
    for (size_t i=0;i<8;++i) CHECK(result[i]==255);
    CHECK(result[8]==1);
    raft_vote_response_t actual = {0};
    CHECK(raft_vote_decode_response(result, sizeof(result), &actual) == RAFT_OK);
    CHECK(actual.term == UINT64_MAX && actual.vote_granted == 1);
    result[8]=2;
    CHECK(raft_vote_decode_response(result, sizeof(result), &actual) != RAFT_OK);
    CHECK(actual.term == UINT64_MAX && actual.vote_granted == 1);
    CHECK(raft_vote_decode_response(result, sizeof(result)-1, &actual) != RAFT_OK);
    response.vote_granted=-1;
    CHECK(raft_vote_encode_response(&response, result) != RAFT_OK);
    raft_vote_request_t invalid = {0};
    CHECK(raft_vote_encode_request(&invalid, bytes) != RAFT_OK);
    invalid=(raft_vote_request_t){.term=2,.candidate_id=3,.last_log_index=0,.last_log_term=1};
    CHECK(raft_vote_encode_request(&invalid, bytes) != RAFT_OK);
    invalid=(raft_vote_request_t){.term=2,.candidate_id=3,.last_log_index=1,.last_log_term=3};
    CHECK(raft_vote_encode_request(&invalid, bytes) != RAFT_OK);
    puts("PASS vote codec: byte order, round trip, malformed payloads");
}

static void test_terms_and_duplicates(void)
{
    raft_node_t node;
    fresh(&node, 1, 2);
    raft_vote_response_t out = {0};
    raft_vote_request_t req = {1,2,0,0};
    uint64_t deadline = node.election_timer.deadline_ms;
    CHECK(raft_vote_handle(&node,&req,1050,&out) == RAFT_OK);
    CHECK(!out.vote_granted && out.term==2 && node.current_term==2);
    CHECK(node.election_timer.deadline_ms == deadline);
    req.term=2;
    CHECK(raft_vote_handle(&node,&req,1100,&out) == RAFT_OK);
    CHECK(out.vote_granted && out.term==2 && node.voted_for==2);
    CHECK(node.election_timer.deadline_ms > 1100);
    CHECK(raft_vote_handle(&node,&req,1150,&out) == RAFT_OK);
    CHECK(out.vote_granted && node.voted_for==2 && node.election_timer.deadline_ms>1150);
    req.candidate_id=3;
    uint64_t deadline_before_reject = node.election_timer.deadline_ms;
    CHECK(raft_vote_handle(&node,&req,1200,&out) == RAFT_OK);
    CHECK(!out.vote_granted && node.voted_for==2);
    CHECK(node.election_timer.deadline_ms==deadline_before_reject);
    req.term=3;
    CHECK(raft_vote_handle(&node,&req,1250,&out) == RAFT_OK);
    CHECK(out.vote_granted && out.term==3 && node.voted_for==3);
    CHECK(node.role==RAFT_FOLLOWER && node.current_term==3);
    CHECK(raft_node_is_valid(&node));
    raft_node_destroy(&node);
    puts("PASS vote logic: terms, single vote per term, repeated candidate, timer");
}

static void test_log_freshness(void)
{
    raft_node_t node;
    fresh(&node, 1, 6);
    CHECK(raft_log_append(&node.log, 3, "abc", 3) == RAFT_OK);
    CHECK(raft_log_append(&node.log, 5, "def", 3) == RAFT_OK);
    raft_vote_response_t out = {0};
    raft_vote_request_t req = {6,2,100,4};
    CHECK(raft_vote_handle(&node,&req,1050,&out) == RAFT_OK);
    CHECK(!out.vote_granted && node.voted_for == RAFT_NO_VOTE);
    req.last_log_index=1; req.last_log_term=5;
    CHECK(raft_vote_handle(&node,&req,1050,&out) == RAFT_OK);
    CHECK(!out.vote_granted);
    req.last_log_index=2;
    CHECK(raft_vote_handle(&node,&req,1050,&out) == RAFT_OK);
    CHECK(out.vote_granted && node.voted_for==2);
    req.term=7; req.candidate_id=3; req.last_log_index=1; req.last_log_term=5;
    uint64_t deadline = node.election_timer.deadline_ms;
    CHECK(raft_vote_handle(&node,&req,1100,&out) == RAFT_OK);
    CHECK(!out.vote_granted && out.term==7 && node.current_term==7);
    CHECK(node.voted_for == RAFT_NO_VOTE && node.election_timer.deadline_ms==deadline);
    req.last_log_term=6;
    CHECK(raft_vote_handle(&node,&req,1100,&out) == RAFT_OK);
    CHECK(out.vote_granted && node.voted_for==3);
    raft_node_destroy(&node);
    puts("PASS vote logic: term-before-index log comparison, higher-term rejection");
}

static void test_role_transition_and_invalid(void)
{
    raft_node_t node;
    fresh(&node,1,4);
    node.role=RAFT_CANDIDATE;
    node.voted_for=1;
    raft_vote_request_t req = {5,2,0,0};
    raft_vote_response_t rsp = {0};
    CHECK(raft_vote_handle(&node,&req,1110,&rsp) == RAFT_OK);
    CHECK(rsp.vote_granted && node.role==RAFT_FOLLOWER && node.voted_for==2);
    req.term=0;
    CHECK(raft_vote_handle(&node,&req,1111,&rsp) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(node.current_term==5 && node.voted_for==2);
    CHECK(raft_vote_handle(NULL,&req,1111,&rsp) == RAFT_ERR_INVALID_ARGUMENT);
    raft_node_destroy(&node);
    puts("PASS vote logic: higher-term step-down and invalid requests");
}

int main(void)
{
    test_wire(); test_terms_and_duplicates(); test_log_freshness();
    test_role_transition_and_invalid();
    printf("All Phase 8 vote unit tests passed (%u assertions).\n",checks);
    return 0;
}
