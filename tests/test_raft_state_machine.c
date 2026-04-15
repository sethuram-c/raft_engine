#include "raft_state_machine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long checks = 0;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    exit(EXIT_FAILURE); } ++checks; } while (0)

static void init_node(raft_node_t *node)
{
    CHECK(raft_node_init(node, 1) == RAFT_OK);
    node->current_term = 3;
}

static void push(raft_node_t *node, const void *cmd, size_t length)
{
    CHECK(raft_log_append(&node->log, node->current_term, cmd, length) == RAFT_OK);
}

static void expect(raft_kv_store_t *kv, const char *key, const void *value, size_t length)
{
    const uint8_t *actual = NULL;
    size_t count = 999;
    CHECK(raft_kv_get(kv, (const uint8_t *)key, strlen(key), &actual, &count) == 1);
    CHECK(count == length);
    if (length) CHECK(memcmp(actual, value, length) == 0);
}

static void test_ordered_apply(void)
{
    raft_node_t node;
    raft_kv_store_t kv;
    raft_kv_init(&kv);
    init_node(&node);
    const char cmd1[] = "SET answer 41";
    const char cmd2[] = "SET answer 42";
    const char cmd3[] = "DEL answer";
    const char cmd4[] = "SET answer 43";
    push(&node, cmd1, sizeof(cmd1)-1);
    push(&node, cmd2, sizeof(cmd2)-1);
    push(&node, cmd3, sizeof(cmd3)-1);
    push(&node, cmd4, sizeof(cmd4)-1);
    raft_index_t applied = 999;
    CHECK(raft_apply_committed(&node, &kv, &applied) == RAFT_OK);
    CHECK(applied == 0 && node.last_applied == 0 && kv.count == 0);
    node.commit_index = 1;
    CHECK(raft_apply_committed(&node, &kv, &applied) == RAFT_OK);
    CHECK(applied == 1 && node.last_applied == 1);
    expect(&kv, "answer", "41", 2);
    CHECK(raft_apply_committed(&node, &kv, &applied) == RAFT_OK);
    CHECK(applied == 0 && kv.executed_commands == 1);
    node.commit_index = 3;
    CHECK(raft_apply_committed(&node, &kv, &applied) == RAFT_OK);
    CHECK(applied == 2 && node.last_applied == 3 && kv.count == 0);
    node.commit_index = 4;
    CHECK(raft_apply_committed(&node, &kv, &applied) == RAFT_OK);
    CHECK(applied == 1 && node.last_applied == 4);
    expect(&kv, "answer", "43", 2);
    CHECK(kv.executed_commands == 4 && kv.ignored_commands == 0);
    raft_node_destroy(&node);
    raft_kv_destroy(&kv);
}

static void test_binary_empty_and_invalid(void)
{
    raft_node_t node;
    raft_kv_store_t kv;
    raft_kv_init(&kv);
    init_node(&node);
    const uint8_t binary[] = {'S','E','T',' ','b',' ','A',0,'B',255};
    push(&node, binary, sizeof(binary));
    push(&node, "SET empty ", 10);
    push(&node, "SET missing", 11);
    push(&node, "DEL missing", 11);
    push(&node, "SET bad/key val", 15);
    push(&node, "OTHER ignored", 13);
    push(&node, NULL, 0); /* Heartbeat-equivalent empty log command: no-op. */
    node.commit_index = raft_log_last_index(&node.log);
    raft_index_t applied = 0;
    CHECK(raft_apply_committed(&node, &kv, &applied) == RAFT_OK);
    CHECK(applied == 7 && node.last_applied == 7);
    expect(&kv, "b", "A\0B\xff", 4);
    expect(&kv, "empty", NULL, 0);
    CHECK(kv.executed_commands == 3 && kv.ignored_commands == 4);
    CHECK(raft_kv_get(&kv, (const uint8_t *)"missing", 7, NULL, NULL) == -1);
    const uint8_t *value = NULL;
    size_t length = 111;
    CHECK(raft_kv_get(&kv, (const uint8_t *)"missing", 7, &value, &length) == 0);
    CHECK(value == NULL && length == 0);
    CHECK(raft_kv_get(&kv, (const uint8_t *)"bad/key", 7, &value, &length) == -1);
    raft_kv_destroy(&kv);
    raft_node_destroy(&node);
}

static void test_guards_and_limits(void)
{
    raft_node_t node;
    raft_kv_store_t kv;
    raft_kv_init(&kv);
    init_node(&node);
    raft_index_t count = 91;
    CHECK(raft_apply_committed(NULL, &kv, &count) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_apply_committed(&node, NULL, &count) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_apply_committed(&node, &kv, NULL) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(count == 91);
    CHECK(!raft_kv_valid_key(NULL, 1));
    CHECK(!raft_kv_valid_key((const uint8_t *)"", 0));
    CHECK(!raft_kv_valid_key((const uint8_t *)"bad key", 7));
    CHECK(raft_kv_valid_key((const uint8_t *)"a_B-7.x", 7));
    char long_key[RAFT_KV_MAX_KEY_BYTES + 2];
    memset(long_key, 'x', sizeof(long_key));
    CHECK(!raft_kv_valid_key((const uint8_t *)long_key, sizeof(long_key)));
    uint8_t overly_long[4 + 1 + 1 + 1 + RAFT_KV_MAX_VALUE_BYTES + 1];
    memcpy(overly_long, "SET k ", 6);
    memset(overly_long+6, 'v', sizeof(overly_long)-6);
    push(&node, overly_long, sizeof(overly_long));
    node.commit_index = 1;
    CHECK(raft_apply_committed(&node, &kv, &count) == RAFT_OK);
    CHECK(count == 1 && node.last_applied == 1 && kv.ignored_commands == 1);
    /* Corrupt invariant: application must refuse and leave state intact. */
    node.commit_index = 2;
    count = 77;
    CHECK(raft_apply_committed(&node, &kv, &count) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(count == 77 && node.last_applied == 1);
    raft_node_destroy(&node);
    raft_kv_destroy(&kv);
}

static void test_all_keys_and_delete(void)
{
    raft_node_t node;
    raft_kv_store_t kv;
    raft_kv_init(&kv);
    init_node(&node);
    char cmd[50];
    for (unsigned i = 0; i < RAFT_KV_MAX_KEYS; ++i) {
        int len = snprintf(cmd, sizeof(cmd), "SET key%u value", i);
        CHECK(len > 0 && (size_t)len < sizeof(cmd));
        push(&node, cmd, (size_t)len);
    }
    push(&node, "SET overflow value", 18);
    push(&node, "DEL key0", 8);
    push(&node, "SET replacement yes", 19);
    node.commit_index = raft_log_last_index(&node.log);
    raft_index_t done = 0;
    CHECK(raft_apply_committed(&node, &kv, &done) == RAFT_OK);
    CHECK(done == RAFT_KV_MAX_KEYS + 3);
    CHECK(kv.count == RAFT_KV_MAX_KEYS && kv.ignored_commands == 1);
    expect(&kv, "replacement", "yes", 3);
    const uint8_t *p = NULL;
    size_t n = 0;
    CHECK(raft_kv_get(&kv, (const uint8_t *)"key0", 4, &p, &n) == 0);
    raft_kv_destroy(&kv);
    raft_node_destroy(&node);
}

int main(void)
{
    test_ordered_apply();
    test_binary_empty_and_invalid();
    test_guards_and_limits();
    test_all_keys_and_delete();
    printf("PASS: %lu Phase 15 ordered state-machine assertions\n", checks);
    return 0;
}
