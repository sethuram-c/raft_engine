#define _POSIX_C_SOURCE 200809L
#include "raft_snapshot.h"
#include "raft_client_write.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#define SNAP_HEADER 48u
static const uint8_t snap_magic[8] = {'R','A','F','T','1','9','S','N'};

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (24u-8u*i));
}
static void put64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (56u-8u*i));
}
static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static uint32_t get32(const uint8_t *p)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i) v = (v << 8) | p[i];
    return v;
}
static uint64_t get64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}
static uint32_t checksum(const uint8_t *p, size_t n)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (unsigned b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void raft_snapshot_init(raft_snapshot_t *s)
{
    if (!s) return;
    s->last_index = 0;
    s->last_term = 0;
    raft_kv_init(&s->state);
}
void raft_snapshot_destroy(raft_snapshot_t *s)
{
    if (!s) return;
    raft_kv_destroy(&s->state);
    s->last_index = 0;
    s->last_term = 0;
}

static int state_valid(const raft_snapshot_t *s)
{
    if (!s || !s->last_index || !s->last_term ||
        s->state.count > RAFT_KV_MAX_KEYS ||
        s->state.session_count > RAFT_KV_MAX_SESSIONS ||
        s->state.executed_commands > s->last_index ||
        s->state.ignored_commands != s->last_index - s->state.executed_commands)
        return 0;
    for (size_t i = 0; i < s->state.count; ++i) {
        const raft_kv_item_t *item = &s->state.items[i];
        size_t len = strnlen(item->key, sizeof(item->key));
        if (len == sizeof(item->key) ||
            !raft_kv_valid_key((const uint8_t *)item->key, len) ||
            item->value_len > RAFT_KV_MAX_VALUE_BYTES ||
            (item->value_len && !item->value)) return 0;
        for (size_t j = 0; j < i; ++j)
            if (strcmp(item->key, s->state.items[j].key) == 0) return 0;
    }
    for (size_t i=0; i<s->state.session_count; ++i) {
        const raft_kv_session_t *rec=&s->state.sessions[i];
        if (!rec->client_id || !rec->request_id || !rec->term ||
            !rec->index || rec->index>s->last_index || rec->term>s->last_term ||
            !raft_client_write_valid_command(rec->command,rec->command_len))
            return 0;
        for (size_t j=0; j<i; ++j)
            if (s->state.sessions[j].client_id==rec->client_id) return 0;
    }
    return 1;
}

static raft_status_t deep_copy(const raft_snapshot_t *s, raft_snapshot_t *out)
{
    raft_snapshot_t tmp;
    raft_snapshot_init(&tmp);
    tmp.last_index = s->last_index;
    tmp.last_term = s->last_term;
    tmp.state.session_count = s->state.session_count;
    memcpy(tmp.state.sessions,s->state.sessions,
           s->state.session_count*sizeof(s->state.sessions[0]));
    tmp.state.executed_commands = s->state.executed_commands;
    tmp.state.ignored_commands = s->state.ignored_commands;
    for (size_t i = 0; i < s->state.count; ++i) {
        const raft_kv_item_t *src = &s->state.items[i];
        raft_kv_item_t *dst = &tmp.state.items[tmp.state.count];
        size_t klen = strlen(src->key);
        memcpy(dst->key, src->key, klen + 1);
        if (src->value_len) {
            dst->value = malloc(src->value_len);
            if (!dst->value) { raft_snapshot_destroy(&tmp); return RAFT_ERR_NO_MEMORY; }
            memcpy(dst->value, src->value, src->value_len);
        }
        dst->value_len = src->value_len;
        ++tmp.state.count;
    }
    raft_snapshot_destroy(out);
    *out = tmp;
    return RAFT_OK;
}

raft_status_t raft_snapshot_capture(const raft_node_t *node,
                                     const raft_kv_store_t *store,
                                     raft_snapshot_t *out)
{
    if (!node || !store || !out || !raft_node_is_valid(node) ||
        !node->last_applied || node->last_applied > node->commit_index)
        return RAFT_ERR_INVALID_ARGUMENT;
    raft_term_t term;
    if (raft_log_term_at(&node->log, node->last_applied, &term) != RAFT_OK)
        return RAFT_ERR_INVALID_ARGUMENT;
    raft_snapshot_t view;
    raft_snapshot_init(&view);
    view.last_index = node->last_applied;
    view.last_term = term;
    view.state = *store; /* borrowed view; never destroyed */
    if (!state_valid(&view)) return RAFT_ERR_INVALID_ARGUMENT;
    return deep_copy(&view, out);
}

raft_status_t raft_snapshot_encode(const raft_snapshot_t *s,
                                    uint8_t **out_bytes, size_t *out_length)
{
    if (!out_bytes || !out_length || !state_valid(s))
        return RAFT_ERR_INVALID_ARGUMENT;
    size_t size = SNAP_HEADER + 4u;
    for (size_t i = 0; i < s->state.count; ++i) {
        const raft_kv_item_t *it = &s->state.items[i];
        size_t k = strlen(it->key);
        if (size > RAFT_SNAPSHOT_MAX_BYTES - 4u - k - it->value_len)
            return RAFT_ERR_OUT_OF_RANGE;
        size += 4u + k + it->value_len;
    }
    /* Version 1 remains byte-identical for existing snapshots. Version 2
     * appends an explicit count and fixed-size client dedup records. */
    if (s->state.session_count) {
        size_t extra=2u;
        for (size_t i=0;i<s->state.session_count;++i)
            extra += 34u + s->state.sessions[i].command_len;
        if (size > RAFT_SNAPSHOT_MAX_BYTES-extra) return RAFT_ERR_OUT_OF_RANGE;
        size += extra;
    }
    uint8_t *data = malloc(size);
    if (!data) return RAFT_ERR_NO_MEMORY;
    memcpy(data, snap_magic, 8);
    put32(data + 8, s->state.session_count ? 2u : 1u);
    put64(data + 12, s->last_index);
    put64(data + 20, s->last_term);
    put64(data + 28, s->state.executed_commands);
    put64(data + 36, s->state.ignored_commands);
    put32(data + 44, (uint32_t)s->state.count);
    size_t at = SNAP_HEADER;
    for (size_t i = 0; i < s->state.count; ++i) {
        const raft_kv_item_t *it = &s->state.items[i];
        size_t k = strlen(it->key);
        put16(data + at, (uint16_t)k);
        put16(data + at + 2, (uint16_t)it->value_len);
        at += 4;
        memcpy(data + at, it->key, k); at += k;
        if (it->value_len) memcpy(data + at, it->value, it->value_len);
        at += it->value_len;
    }
    if (s->state.session_count) {
        put16(data+at,(uint16_t)s->state.session_count); at+=2;
        for (size_t i=0; i<s->state.session_count; ++i) {
            const raft_kv_session_t *r=&s->state.sessions[i];
            put64(data+at,r->client_id); at+=8;
            put64(data+at,r->request_id); at+=8;
            put64(data+at,r->index); at+=8;
            put64(data+at,r->term); at+=8;
            put16(data+at,r->command_len); at+=2;
            memcpy(data+at,r->command,r->command_len); at+=r->command_len;
        }
    }
    put32(data + at, checksum(data, at));
    *out_bytes = data; *out_length = size;
    return RAFT_OK;
}

raft_status_t raft_snapshot_decode(const uint8_t *data, size_t length,
                                    raft_snapshot_t *out)
{
    if (!data || !out || length < SNAP_HEADER + 4 ||
        length > RAFT_SNAPSHOT_MAX_BYTES ||
        memcmp(data, snap_magic, 8) != 0 ||
        (get32(data + 8) != 1 && get32(data + 8) != 2) ||
        get32(data + length - 4) != checksum(data, length - 4))
        return RAFT_ERR_INVALID_ARGUMENT;
    raft_snapshot_t tmp;
    raft_snapshot_init(&tmp);
    tmp.last_index = get64(data + 12);
    tmp.last_term = get64(data + 20);
    tmp.state.executed_commands = get64(data + 28);
    tmp.state.ignored_commands = get64(data + 36);
    uint32_t count = get32(data + 44);
    if (count > RAFT_KV_MAX_KEYS) return RAFT_ERR_INVALID_ARGUMENT;
    size_t at = SNAP_HEADER;
    for (uint32_t i = 0; i < count; ++i) {
        if (at > length - 4 || length - 4 - at < 4) goto bad;
        size_t k = get16(data + at), v = get16(data + at + 2);
        at += 4;
        if (k == 0 || k > RAFT_KV_MAX_KEY_BYTES ||
            v > RAFT_KV_MAX_VALUE_BYTES || k > length - 4 - at ||
            v > length - 4 - at - k) goto bad;
        raft_kv_item_t *it = &tmp.state.items[tmp.state.count];
        if (!raft_kv_valid_key(data + at, k)) goto bad;
        memcpy(it->key, data + at, k); it->key[k] = '\0'; at += k;
        if (v) {
            it->value = malloc(v);
            if (!it->value) { raft_snapshot_destroy(&tmp); return RAFT_ERR_NO_MEMORY; }
            memcpy(it->value, data + at, v);
        }
        it->value_len = v;
        ++tmp.state.count;
        at += v;
    }
    if (get32(data+8)==2) {
        if (at>length-4 || length-4-at<2) goto bad;
        size_t count_sessions=get16(data+at); at+=2;
        if (count_sessions==0 || count_sessions>RAFT_KV_MAX_SESSIONS ||
            count_sessions>(length-4-at)/34u) goto bad;
        for (size_t i=0; i<count_sessions; ++i) {
            if (at > length-4 || length-4-at < 34u) goto bad;
            raft_kv_session_t *r=&tmp.state.sessions[i];
            r->client_id=get64(data+at); at+=8;
            r->request_id=get64(data+at); at+=8;
            r->index=get64(data+at); at+=8;
            r->term=get64(data+at); at+=8;
            r->command_len=get16(data+at); at+=2;
            if (r->command_len > RAFT_KV_MAX_VALUE_BYTES ||
                r->command_len > length-4-at) goto bad;
            memcpy(r->command,data+at,r->command_len); at+=r->command_len;
        }
        tmp.state.session_count=count_sessions;
    }
    if (at != length - 4 || !state_valid(&tmp)) goto bad;
    raft_snapshot_destroy(out);
    *out = tmp;
    return RAFT_OK;
bad:
    raft_snapshot_destroy(&tmp);
    return RAFT_ERR_INVALID_ARGUMENT;
}

raft_status_t raft_snapshot_restore_fresh(const raft_snapshot_t *s,
                                          raft_node_t *node,
                                          raft_kv_store_t *store)
{
    if (!state_valid(s) || !node || !store || !raft_node_is_valid(node) ||
        node->log.base_index != 0 || node->log.count != 0 ||
        node->commit_index != 0 || node->last_applied != 0 ||
        node->current_term < s->last_term) return RAFT_ERR_INVALID_ARGUMENT;
    raft_snapshot_t copy;
    raft_snapshot_init(&copy);
    raft_status_t status = deep_copy(s, &copy);
    if (status != RAFT_OK) return status;
    raft_kv_destroy(store);
    *store = copy.state;
    raft_kv_init(&copy.state);
    raft_snapshot_destroy(&copy);
    node->log.base_index = s->last_index;
    node->log.base_term = s->last_term;
    node->commit_index = s->last_index;
    node->last_applied = s->last_index;
    return RAFT_OK;
}

raft_status_t raft_snapshot_compact_applied(raft_node_t *node,
                                            const raft_snapshot_t *s)
{
    if (!node || !state_valid(s) || !raft_node_is_valid(node) ||
        s->last_index > node->last_applied ||
        s->last_index > node->commit_index ||
        !raft_log_matches(&node->log, s->last_index, s->last_term))
        return RAFT_ERR_INVALID_ARGUMENT;
    return raft_log_compact_prefix(&node->log, s->last_index, s->last_term);
}

static int safe_name(const char *name)
{
    if (!name || !name[0] || strlen(name) > 120 || name[0] == '.') return 0;
    for (const char *p = name; *p; ++p)
        if ((*p < 'A' || *p > 'Z') && (*p < 'a' || *p > 'z') &&
            (*p < '0' || *p > '9') && *p != '_' && *p != '-' && *p != '.')
            return 0;
    return 1;
}
static int write_all(int fd, const uint8_t *p, size_t n)
{
    size_t at = 0;
    while (at < n) {
        ssize_t got = write(fd, p + at, n - at);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return -1;
        at += (size_t)got;
    }
    return 0;
}
static int read_all(int fd, uint8_t *p, size_t n)
{
    size_t at = 0;
    while (at < n) {
        ssize_t got = read(fd, p + at, n - at);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return -1;
        at += (size_t)got;
    }
    return 0;
}

int raft_snapshot_write_file(const char *directory, const char *name,
                             const raft_snapshot_t *snapshot)
{
    if (!directory || !safe_name(name)) return -1;
    uint8_t *data = NULL; size_t length = 0;
    if (raft_snapshot_encode(snapshot, &data, &length) != RAFT_OK) return -1;
    int dir = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) { free(data); return -1; }
    char tmp[160];
    (void)snprintf(tmp, sizeof(tmp), ".%s.tmp.%ld", name, (long)getpid());
    int fd = openat(dir, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    int ok = fd >= 0 && write_all(fd, data, length) == 0 && fsync(fd) == 0;
    if (fd >= 0 && close(fd) != 0) ok = 0;
    free(data);
    if (ok && renameat(dir, tmp, dir, name) == 0 && fsync(dir) == 0) {
        (void)close(dir); return 0;
    }
    (void)unlinkat(dir, tmp, 0);
    (void)close(dir);
    return -1;
}

int raft_snapshot_read_file(const char *directory, const char *name,
                            raft_snapshot_t *out)
{
    if (!directory || !out || !safe_name(name)) return -1;
    int dir = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) return -1;
    int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    (void)close(dir);
    if (fd < 0) return -1;
    struct stat st;
    int ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
             st.st_size >= (off_t)(SNAP_HEADER + 4) &&
             st.st_size <= (off_t)RAFT_SNAPSHOT_MAX_BYTES;
    if (!ok) { (void)close(fd); return -1; }
    size_t length = (size_t)st.st_size;
    uint8_t *data = malloc(length);
    if (!data) { (void)close(fd); return -1; }
    ok = read_all(fd, data, length) == 0;
    uint8_t extra;
    ssize_t remainder = read(fd, &extra, 1);
    if (close(fd) != 0) ok = 0;
    if (remainder != 0) ok = 0;
    if (ok) ok = raft_snapshot_decode(data, length, out) == RAFT_OK;
    free(data);
    return ok ? 0 : -1;
}
