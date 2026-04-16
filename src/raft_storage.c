#define _POSIX_C_SOURCE 200809L
#include "raft_storage.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif

static const uint8_t magic[8] = {'R','A','F','T','1','6','D','B'};
#define HEADER_BYTES 40u

static void errout(char *error, size_t cap, const char *fmt, ...)
{
    if (!error || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(error, cap, fmt, ap);
    va_end(ap);
}

static void put32(uint8_t *p, uint32_t x)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(x >> (24u - i * 8u));
}
static void put64(uint8_t *p, uint64_t x)
{
    for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(x >> (56u - i * 8u));
}
static uint32_t get32(const uint8_t *p)
{
    uint32_t x = 0;
    for (unsigned i = 0; i < 4; ++i) x = (x << 8) | p[i];
    return x;
}
static uint64_t get64(const uint8_t *p)
{
    uint64_t x = 0;
    for (unsigned i = 0; i < 8; ++i) x = (x << 8) | p[i];
    return x;
}
static uint32_t checksum(const uint8_t *p, size_t count)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < count; ++i) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static int read_full(int fd, uint8_t *buf, size_t size)
{
    size_t done = 0;
    while (done < size) {
        ssize_t n = read(fd, buf + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}
static int write_full(int fd, const uint8_t *buf, size_t size)
{
    size_t done = 0;
    while (done < size) {
        ssize_t n = write(fd, buf + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

int raft_storage_open(raft_storage_t *store, const char *directory,
                      raft_node_id_t id, char *error, size_t cap)
{
    if (!store || !directory || !*directory || id == RAFT_NO_VOTE) {
        errout(error, cap, "invalid storage arguments");
        return -1;
    }
    store->directory_fd = -1;
    store->lock_fd = -1;
    store->id = id;
    if (mkdir(directory, 0700) < 0 && errno != EEXIST) {
        errout(error, cap, "mkdir storage: %s", strerror(errno));
        return -1;
    }
    int dfd = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0) {
        errout(error, cap, "open storage directory: %s", strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(dfd, &st) != 0 || !S_ISDIR(st.st_mode)) {
        errout(error, cap, "storage path is not a directory");
        (void)close(dfd);
        return -1;
    }
    char lock_name[48];
    (void)snprintf(lock_name, sizeof(lock_name), "node-%" PRIu32 ".lock", id);
    (void)snprintf(store->data_name, sizeof(store->data_name), "node-%" PRIu32 ".raft", id);
    (void)snprintf(store->temp_name, sizeof(store->temp_name), "node-%" PRIu32 ".tmp", id);
    int lfd = openat(dfd, lock_name, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lfd < 0 || flock(lfd, LOCK_EX | LOCK_NB) < 0) {
        errout(error, cap, "node storage lock unavailable: %s", strerror(errno));
        if (lfd >= 0) (void)close(lfd);
        (void)close(dfd);
        return -1;
    }
    store->directory_fd = dfd;
    store->lock_fd = lfd;
    if (error && cap) error[0] = '\0';
    return 0;
}

void raft_storage_close(raft_storage_t *store)
{
    if (!store) return;
    if (store->lock_fd >= 0) (void)close(store->lock_fd);
    if (store->directory_fd >= 0) (void)close(store->directory_fd);
    store->directory_fd = -1;
    store->lock_fd = -1;
}

int raft_storage_save(raft_storage_t *store, const raft_node_t *node,
                      char *error, size_t cap)
{
    if (!store || store->directory_fd < 0 || store->lock_fd < 0 ||
        !node || node->id != store->id || !raft_node_is_valid(node) ||
        node->log.count > RAFT_STORAGE_MAX_ENTRIES ||
        node->log.count > UINT32_MAX) {
        errout(error, cap, "invalid or oversized durable node state");
        return -1;
    }
    size_t size = HEADER_BYTES + 4;
    for (size_t i = 0; i < node->log.count; ++i) {
        size_t len = node->log.entries[i].command_len;
        if (len > RAFT_STORAGE_MAX_COMMAND || size > RAFT_STORAGE_MAX_BYTES - 12u - len) {
            errout(error, cap, "durable log exceeds snapshot limit");
            return -1;
        }
        size += 12 + len;
    }
    uint8_t *data = malloc(size);
    if (!data) { errout(error, cap, "out of memory"); return -1; }
    memcpy(data, magic, sizeof(magic));
    put32(data + 8, 1);                   /* storage format version */
    put32(data + 12, node->id);
    put64(data + 16, node->current_term);
    put32(data + 24, node->voted_for);
    put64(data + 28, node->commit_index);
    put32(data + 36, (uint32_t)node->log.count);
    size_t at = HEADER_BYTES;
    for (size_t i = 0; i < node->log.count; ++i) {
        const raft_log_entry_t *entry = &node->log.entries[i];
        put64(data + at, entry->term);
        put32(data + at + 8, (uint32_t)entry->command_len);
        at += 12;
        if (entry->command_len) memcpy(data + at, entry->command, entry->command_len);
        at += entry->command_len;
    }
    put32(data + at, checksum(data, at));
    int fd = openat(store->directory_fd, store->temp_name,
                    O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        errout(error, cap, "open temporary storage: %s", strerror(errno));
        free(data);
        return -1;
    }
    int ok = write_full(fd, data, size) == 0 && fsync(fd) == 0;
    int write_errno = errno;
    if (close(fd) != 0) { ok = 0; write_errno = errno; }
    free(data);
    if (!ok) {
        errout(error, cap, "durable write/fsync failed: %s", strerror(write_errno));
        (void)unlinkat(store->directory_fd, store->temp_name, 0);
        return -1;
    }
    if (renameat(store->directory_fd, store->temp_name,
                 store->directory_fd, store->data_name) != 0 ||
        fsync(store->directory_fd) != 0) {
        errout(error, cap, "rename/directory fsync failed: %s", strerror(errno));
        return -1;
    }
    if (error && cap) error[0] = '\0';
    return 0;
}

int raft_storage_load(raft_storage_t *store, raft_node_t *node, int *found,
                      char *error, size_t cap)
{
    if (!store || store->directory_fd < 0 || !node || !found || node->id != store->id) {
        errout(error, cap, "invalid storage load arguments");
        return -1;
    }
    *found = 0;
    int fd = openat(store->directory_fd, store->data_name,
                    O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return 0;
        errout(error, cap, "open saved node: %s", strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size < (off_t)(HEADER_BYTES + 4) ||
        st.st_size > (off_t)RAFT_STORAGE_MAX_BYTES) {
        errout(error, cap, "invalid durable snapshot length/type");
        (void)close(fd);
        return -1;
    }
    size_t size = (size_t)st.st_size;
    uint8_t *data = malloc(size);
    if (!data) {
        errout(error, cap, "out of memory");
        (void)close(fd);
        return -1;
    }
    int ok = read_full(fd, data, size) == 0;
    (void)close(fd);
    if (!ok || memcmp(data, magic, sizeof(magic)) != 0 ||
        get32(data + 8) != 1 || get32(data + 12) != store->id ||
        checksum(data, size - 4) != get32(data + size - 4)) {
        errout(error, cap, "corrupt/mismatched durable snapshot (header or CRC)");
        free(data);
        return -1;
    }
    uint64_t term = get64(data + 16);
    uint32_t vote = get32(data + 24);
    uint64_t committed = get64(data + 28);
    uint32_t count = get32(data + 36);
    if (count > RAFT_STORAGE_MAX_ENTRIES || committed > count) {
        errout(error, cap, "invalid snapshot counters");
        free(data);
        return -1;
    }
    raft_node_t tmp;
    if (raft_node_init(&tmp, store->id) != RAFT_OK) { free(data); return -1; }
    tmp.current_term = term;
    tmp.voted_for = vote;
    tmp.commit_index = committed;
    size_t at = HEADER_BYTES;
    for (uint32_t i = 0; i < count; ++i) {
        if (at > size - 4 || size - 4 - at < 12) { ok = 0; break; }
        uint64_t entry_term = get64(data + at);
        uint32_t len = get32(data + at + 8);
        at += 12;
        if (len > RAFT_STORAGE_MAX_COMMAND || size - 4 - at < len ||
            raft_log_append(&tmp.log, entry_term, data + at, len) != RAFT_OK) {
            ok = 0;
            break;
        }
        at += len;
    }
    if (!ok || at != size - 4 || !raft_node_is_valid(&tmp)) {
        errout(error, cap, "corrupt durable log or inconsistent node state");
        raft_node_destroy(&tmp);
        free(data);
        return -1;
    }
    free(data);
    raft_node_destroy(node);
    *node = tmp;
    *found = 1;
    if (error && cap) error[0] = '\0';
    return 0;
}
