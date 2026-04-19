#define _POSIX_C_SOURCE 200809L
#include "raft_snapshot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

static int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)

static void initial(raft_node_t *n, raft_kv_store_t *kv)
{
    CHECK(raft_node_init(n, 1) == RAFT_OK);
    raft_kv_init(kv);
    n->current_term = 2;
    const uint8_t binary[] = {'S','E','T',' ','b',' ','X',0,'Y'};
    CHECK(raft_log_append(&n->log, 1, "SET a one", 9) == RAFT_OK);
    CHECK(raft_log_append(&n->log, 1, binary, sizeof(binary)) == RAFT_OK);
    CHECK(raft_log_append(&n->log, 2, "DEL a", 5) == RAFT_OK);
    n->commit_index = 2;
    raft_index_t done = 0;
    CHECK(raft_apply_committed(n, kv, &done) == RAFT_OK);
    CHECK(done == 2 && n->last_applied == 2);
}

static void test_codec_and_compaction(void)
{
    raft_node_t n; raft_kv_store_t kv;
    initial(&n, &kv);
    raft_snapshot_t s, got;
    raft_snapshot_init(&s); raft_snapshot_init(&got);
    CHECK(raft_snapshot_capture(&n, &kv, &s) == RAFT_OK);
    CHECK(s.last_index == 2 && s.last_term == 1);
    CHECK(s.state.count == 2 && s.state.executed_commands == 2);
    const uint8_t *val = NULL; size_t len = 0;
    CHECK(raft_kv_get(&s.state, (const uint8_t *)"b", 1, &val, &len) == 1);
    CHECK(len == 3 && val[0] == 'X' && val[1] == 0 && val[2] == 'Y');
    uint8_t *bytes = NULL; size_t size = 0;
    CHECK(raft_snapshot_encode(&s, &bytes, &size) == RAFT_OK);
    CHECK(size > 48 && size < RAFT_SNAPSHOT_MAX_BYTES);
    CHECK(raft_snapshot_decode(bytes, size, &got) == RAFT_OK);
    CHECK(got.last_index == 2 && got.last_term == 1);
    CHECK(raft_kv_get(&got.state, (const uint8_t *)"b", 1, &val, &len) == 1);
    CHECK(len == 3 && memcmp(val, "X\0Y", 3) == 0);
    bytes[24] ^= 1;
    CHECK(raft_snapshot_decode(bytes, size, &got) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(got.last_index == 2); /* failed decoder does not overwrite */
    bytes[24] ^= 1;
    CHECK(raft_snapshot_decode(bytes, size - 1, &got) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_snapshot_decode(bytes, 5, &got) == RAFT_ERR_INVALID_ARGUMENT);
    bytes[8] = 2;
    CHECK(raft_snapshot_decode(bytes, size, &got) == RAFT_ERR_INVALID_ARGUMENT);
    free(bytes);

    CHECK(raft_snapshot_compact_applied(&n, &s) == RAFT_OK);
    CHECK(n.log.base_index == 2 && n.log.base_term == 1);
    CHECK(n.log.count == 1 && raft_log_last_index(&n.log) == 3);
    CHECK(raft_log_at(&n.log, 1) == NULL && raft_log_at(&n.log, 2) == NULL);
    raft_term_t term = 99;
    CHECK(raft_log_term_at(&n.log, 2, &term) == RAFT_OK && term == 1);
    CHECK(raft_log_term_at(&n.log, 1, &term) == RAFT_ERR_OUT_OF_RANGE);
    CHECK(raft_log_matches(&n.log, 2, 1));
    CHECK(raft_log_matches(&n.log, 3, 2));
    raft_index_t rejected = 0;
    CHECK(raft_log_reconcile(&n.log, 1, 1, NULL, 0, 2, &rejected)
          == RAFT_ERR_LOG_MISMATCH);
    CHECK(raft_log_last_term(&n.log, &term) == RAFT_OK && term == 2);
    CHECK(raft_node_is_valid(&n));
    CHECK(raft_snapshot_compact_applied(&n, &s) == RAFT_OK);
    CHECK(raft_log_truncate_from(&n.log, 2) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_log_truncate_from(&n.log, 4) == RAFT_OK);
    CHECK(raft_log_last_index(&n.log) == 3);
    CHECK(raft_log_truncate_from(&n.log, 3) == RAFT_OK);
    CHECK(raft_log_last_index(&n.log) == 2 && n.log.count == 0);
    CHECK(raft_log_append(&n.log, 0, "x", 1) != RAFT_OK);
    CHECK(raft_log_append(&n.log, 1, "SET c 3", 7) == RAFT_OK);
    CHECK(raft_log_last_index(&n.log) == 3);
    CHECK(raft_log_compact_prefix(&n.log, 3, 2) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(n.log.base_index == 2 && raft_log_last_index(&n.log) == 3);
    raft_node_destroy(&n); raft_kv_destroy(&kv);
    raft_snapshot_destroy(&got); raft_snapshot_destroy(&s);
}

static void test_replay(void)
{
    raft_node_t n, restored; raft_kv_store_t kv, replay;
    initial(&n, &kv);
    raft_snapshot_t snapshot;
    raft_snapshot_init(&snapshot);
    CHECK(raft_snapshot_capture(&n, &kv, &snapshot) == RAFT_OK);
    CHECK(raft_snapshot_compact_applied(&n, &snapshot) == RAFT_OK);
    n.commit_index = 3;
    raft_index_t applied = 0;
    CHECK(raft_apply_committed(&n, &kv, &applied) == RAFT_OK);
    CHECK(applied == 1 && n.last_applied == 3);
    const uint8_t *v = NULL; size_t len = 0;
    CHECK(raft_kv_get(&kv, (const uint8_t *)"a", 1, &v, &len) == 0);
    CHECK(raft_node_init(&restored, 2) == RAFT_OK);
    restored.current_term = 2;
    raft_kv_init(&replay);
    CHECK(raft_snapshot_restore_fresh(&snapshot, &restored, &replay) == RAFT_OK);
    CHECK(restored.log.base_index == 2 && restored.log.base_term == 1);
    CHECK(restored.commit_index == 2 && restored.last_applied == 2);
    CHECK(raft_node_is_valid(&restored));
    CHECK(raft_kv_get(&replay, (const uint8_t *)"a", 1, &v, &len) == 1);
    CHECK(raft_log_append(&restored.log, 2, "DEL a", 5) == RAFT_OK);
    restored.commit_index = 3;
    CHECK(raft_apply_committed(&restored, &replay, &applied) == RAFT_OK);
    CHECK(applied == 1 && restored.last_applied == 3);
    CHECK(raft_kv_get(&replay, (const uint8_t *)"a", 1, &v, &len) == 0);
    CHECK(raft_kv_get(&replay, (const uint8_t *)"b", 1, &v, &len) == 1);
    CHECK(len == 3 && memcmp(v, "X\0Y", 3) == 0);
    CHECK(raft_snapshot_restore_fresh(&snapshot, &restored, &replay) == RAFT_ERR_INVALID_ARGUMENT);
    raft_log_entry_t e = {.term = 2, .command = (uint8_t *)"SET c 2", .command_len = 7};
    raft_index_t matched = 0;
    CHECK(raft_log_reconcile(&restored.log, 2, 1, &e, 1, 2, &matched) == RAFT_OK);
    CHECK(matched == 3 && raft_log_last_index(&restored.log) == 3);
    raft_node_destroy(&n); raft_node_destroy(&restored);
    raft_kv_destroy(&kv); raft_kv_destroy(&replay);
    raft_snapshot_destroy(&snapshot);
}

static void test_files(void)
{
    char dir[] = "/tmp/raft-snapshot-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    raft_node_t node; raft_kv_store_t kv;
    initial(&node, &kv);
    raft_snapshot_t s, found;
    raft_snapshot_init(&s); raft_snapshot_init(&found);
    CHECK(raft_snapshot_capture(&node, &kv, &s) == RAFT_OK);
    CHECK(raft_snapshot_write_file(dir, "node-1.snap", &s) == 0);
    CHECK(raft_snapshot_read_file(dir, "node-1.snap", &found) == 0);
    CHECK(found.last_index == 2 && found.last_term == 1);
    CHECK(raft_snapshot_read_file(dir, "missing.snap", &found) != 0);
    CHECK(raft_snapshot_write_file(dir, "../bad", &s) != 0);
    CHECK(raft_snapshot_read_file(dir, "../bad", &found) != 0);
    char filepath[256]; snprintf(filepath,sizeof(filepath),"%s/node-1.snap",dir);
    int fd = open(filepath, O_WRONLY);
    CHECK(fd >= 0);
    CHECK(pwrite(fd, "CORRUPT!", 8, 0) == 8);
    CHECK(close(fd) == 0);
    CHECK(raft_snapshot_read_file(dir, "node-1.snap", &found) != 0);
    CHECK(found.last_index == 2); /* no partially decoded state */
    CHECK(unlink(filepath) == 0);
    CHECK(rmdir(dir) == 0);
    raft_node_destroy(&node); raft_kv_destroy(&kv);
    raft_snapshot_destroy(&s); raft_snapshot_destroy(&found);
}

static void test_rejections(void)
{
    raft_node_t node; raft_kv_store_t kv;
    CHECK(raft_node_init(&node, 1) == RAFT_OK);
    raft_kv_init(&kv);
    raft_snapshot_t s;
    raft_snapshot_init(&s);
    CHECK(raft_snapshot_capture(&node, &kv, &s) == RAFT_ERR_INVALID_ARGUMENT);
    CHECK(raft_snapshot_encode(&s, NULL, NULL) == RAFT_ERR_INVALID_ARGUMENT);
    node.current_term = 1;
    CHECK(raft_log_append(&node.log, 1, "SET a 1", 7) == RAFT_OK);
    node.commit_index = 1;
    node.last_applied = 1; /* counter missing: inconsistent state machine */
    CHECK(raft_snapshot_capture(&node, &kv, &s) == RAFT_ERR_INVALID_ARGUMENT);
    node.last_applied = 0;
    raft_index_t applied;
    CHECK(raft_apply_committed(&node, &kv, &applied) == RAFT_OK);
    CHECK(raft_snapshot_capture(&node, &kv, &s) == RAFT_OK);
    CHECK(raft_snapshot_compact_applied(&node, &s) == RAFT_OK);
    CHECK(raft_log_term_at(&node.log, 0, &s.last_term) == RAFT_ERR_OUT_OF_RANGE);
    raft_node_destroy(&node); raft_kv_destroy(&kv);
    raft_snapshot_destroy(&s);
}

int main(void)
{
    test_codec_and_compaction();
    test_replay();
    test_files();
    test_rejections();
    printf("PASS: %d snapshot, prefix-compaction, replay and file assertions\n", checks);
    return 0;
}
