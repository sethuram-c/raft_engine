#define _POSIX_C_SOURCE 200809L
#include "raft_storage.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
  exit(1); } } while (0)

static void dirpath(char *out, size_t cap)
{
    CHECK(cap > 22);
    (void)snprintf(out, cap, "/tmp/raft16-tests-XXXXXX");
    CHECK(mkdtemp(out) != NULL);
}

int main(void)
{
    char dir[128];
    dirpath(dir, sizeof(dir));
    char error[256] = {0};
    raft_storage_t store;
    CHECK(raft_storage_open(&store, dir, 2, error, sizeof(error)) == 0);

    raft_node_t node;
    CHECK(raft_node_init(&node, 2) == RAFT_OK);
    CHECK(raft_node_start_follower(&node, 10000, 123) == RAFT_OK);
    int found = -1;
    CHECK(raft_storage_load(&store, &node, &found, error, sizeof(error)) == 0);
    CHECK(found == 0);
    CHECK(raft_storage_save(&store, &node, error, sizeof(error)) == 0);
    printf("PASS storage: first boot and empty snapshot\n");

    node.current_term = 8;
    node.voted_for = 3;
    const uint8_t binary[] = {'S','E','T',' ','x',' ','A',0,'B',255};
    CHECK(raft_log_append(&node.log, 6, binary, sizeof(binary)) == RAFT_OK);
    CHECK(raft_log_append(&node.log, 8, "DEL x", 5) == RAFT_OK);
    node.commit_index = 1;
    CHECK(raft_storage_save(&store, &node, error, sizeof(error)) == 0);
    raft_node_t reloaded;
    CHECK(raft_node_init(&reloaded, 2) == RAFT_OK);
    CHECK(raft_storage_load(&store, &reloaded, &found, error, sizeof(error)) == 0);
    CHECK(found == 1 && reloaded.id == 2 && reloaded.role == RAFT_FOLLOWER);
    CHECK(reloaded.current_term == 8 && reloaded.voted_for == 3);
    CHECK(reloaded.commit_index == 1 && reloaded.last_applied == 0);
    CHECK(raft_log_last_index(&reloaded.log) == 2);
    const raft_log_entry_t *a = raft_log_at(&reloaded.log, 1);
    CHECK(a && a->term == 6 && a->command_len == sizeof(binary));
    CHECK(memcmp(a->command, binary, sizeof(binary)) == 0);
    CHECK(raft_log_at(&reloaded.log, 2)->term == 8);
    printf("PASS storage: term, vote, commit, ordered terms and binary log restored\n");

    raft_storage_t duplicate;
    CHECK(raft_storage_open(&duplicate, dir, 2, error, sizeof(error)) != 0);
    raft_storage_t other;
    CHECK(raft_storage_open(&other, dir, 3, error, sizeof(error)) == 0);
    CHECK(raft_storage_load(&other, &node, &found, error, sizeof(error)) != 0);
    raft_storage_close(&other);
    printf("PASS storage: exclusive same-node writer lock and node-id validation\n");

    /* Changing one byte must fail CRC and must NOT mutate existing node. */
    char path[200];
    (void)snprintf(path, sizeof(path), "%s/node-2.raft", dir);
    int fd = open(path, O_RDWR);
    CHECK(fd >= 0);
    CHECK(lseek(fd, 17, SEEK_SET) == 17);
    uint8_t byte;
    CHECK(read(fd, &byte, 1) == 1);
    byte ^= 0x42u;
    CHECK(lseek(fd, 17, SEEK_SET) == 17 && write(fd, &byte, 1) == 1);
    CHECK(close(fd) == 0);
    CHECK(raft_storage_load(&store, &reloaded, &found, error, sizeof(error)) != 0);
    CHECK(reloaded.current_term == 8 && raft_log_last_index(&reloaded.log) == 2);
    CHECK(raft_storage_save(&store, &node, error, sizeof(error)) == 0);
    fd = open(path, O_RDWR);
    CHECK(fd >= 0 && ftruncate(fd, 12) == 0 && close(fd) == 0);
    CHECK(raft_storage_load(&store, &reloaded, &found, error, sizeof(error)) != 0);
    CHECK(reloaded.current_term == 8 && raft_log_last_index(&reloaded.log) == 2);
    printf("PASS storage: CRC and torn-write corruption rejected without partial load\n");

    CHECK(raft_storage_save(&store, &node, error, sizeof(error)) == 0);
    raft_log_destroy(&node.log);
    raft_log_init(&node.log);
    node.commit_index = 0;
    CHECK(raft_storage_save(&store, &node, error, sizeof(error)) == 0);
    CHECK(raft_storage_load(&store, &reloaded, &found, error, sizeof(error)) == 0);
    CHECK(raft_log_last_index(&reloaded.log) == 0 && reloaded.commit_index == 0);
    printf("PASS storage: atomic overwrite with shorter log and no retained suffix\n");

    raft_node_destroy(&reloaded);
    raft_node_destroy(&node);
    raft_storage_close(&store);
    char lock_path[200], tmp_path[200];
    (void)snprintf(lock_path, sizeof(lock_path), "%s/node-2.lock", dir);
    (void)snprintf(tmp_path, sizeof(tmp_path), "%s/node-3.lock", dir);
    CHECK(unlink(path) == 0 && unlink(lock_path) == 0 && unlink(tmp_path) == 0);
    CHECK(rmdir(dir) == 0);
    puts("All Phase 16 durable storage unit tests passed.");
    return 0;
}
