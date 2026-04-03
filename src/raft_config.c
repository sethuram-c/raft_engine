#include "raft_config.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_LINE_SIZE 512

static void set_error(char *error, size_t cap, const char *fmt, ...)
{
    if (error == NULL || cap == 0) return;
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(error, cap, fmt, args);
    va_end(args);
}

static char *trim(char *text)
{
    while (isspace((unsigned char)*text)) ++text;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) --end;
    *end = '\0';
    return text;
}

static int parse_u32(const char *text, uint32_t *value)
{
    if (text == NULL || *text == '\0' || !isdigit((unsigned char)*text)) return 0;
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' ||
        parsed == 0 || parsed > UINT32_MAX) return 0;
    *value = (uint32_t)parsed;
    return 1;
}

static int valid_ipv4(const char *text)
{
    struct in_addr address;
    return inet_pton(AF_INET, text, &address) == 1;
}

static int parse_peer(char *text, raft_peer_config_t *peer)
{
    char *first = strchr(text, ',');
    if (first == NULL) return 0;
    *first++ = '\0';
    char *second = strchr(first, ',');
    if (second == NULL) return 0;
    *second++ = '\0';
    if (strchr(second, ',') != NULL) return 0;
    char *id_text = trim(text);
    char *address = trim(first);
    char *port_text = trim(second);
    uint32_t id, port;
    if (!parse_u32(id_text, &id) || !parse_u32(port_text, &port) ||
        port > UINT16_MAX || strlen(address) >= sizeof(peer->ipv4) ||
        !valid_ipv4(address)) return 0;
    peer->id = (raft_node_id_t)id;
    (void)snprintf(peer->ipv4, sizeof(peer->ipv4), "%s", address);
    peer->port = (uint16_t)port;
    return 1;
}

const raft_peer_config_t *raft_config_peer_for_id(
    const raft_cluster_config_t *config, raft_node_id_t id)
{
    if (config == NULL || id == RAFT_NO_VOTE ||
        config->peer_count > RAFT_CLUSTER_SIZE) return NULL;
    for (size_t i = 0; i < config->peer_count; ++i)
        if (config->peers[i].id == id) return &config->peers[i];
    return NULL;
}

raft_config_status_t raft_config_validate(const raft_cluster_config_t *config,
    char *error, size_t error_size)
{
    if (config == NULL) {
        set_error(error, error_size, "null configuration");
        return RAFT_CONFIG_INVALID_ARGUMENT;
    }
    if (config->node_id == RAFT_NO_VOTE || config->peer_count != RAFT_CLUSTER_SIZE) {
        set_error(error, error_size, "expected local node ID and exactly 3 peers");
        return RAFT_CONFIG_VALIDATION_ERROR;
    }
    for (size_t i = 0; i < config->peer_count; ++i) {
        const raft_peer_config_t *p = &config->peers[i];
        if (p->id == RAFT_NO_VOTE || p->port == 0 ||
            memchr(p->ipv4, '\0', sizeof(p->ipv4)) == NULL ||
            !valid_ipv4(p->ipv4)) {
            set_error(error, error_size, "invalid peer at position %zu", i + 1);
            return RAFT_CONFIG_VALIDATION_ERROR;
        }
        for (size_t j = 0; j < i; ++j) {
            const raft_peer_config_t *other = &config->peers[j];
            if (other->id == p->id) {
                set_error(error, error_size, "duplicate peer ID %u", p->id);
                return RAFT_CONFIG_VALIDATION_ERROR;
            }
            /* Compare numeric address rather than textual spelling. */
            struct in_addr a, b;
            (void)inet_pton(AF_INET, p->ipv4, &a);
            (void)inet_pton(AF_INET, other->ipv4, &b);
            if (p->port == other->port && a.s_addr == b.s_addr) {
                set_error(error, error_size, "duplicate endpoint for peers %u and %u",
                    other->id, p->id);
                return RAFT_CONFIG_VALIDATION_ERROR;
            }
        }
    }
    if (raft_config_peer_for_id(config, config->node_id) == NULL) {
        set_error(error, error_size, "local node ID %u is not in the peer list",
            config->node_id);
        return RAFT_CONFIG_VALIDATION_ERROR;
    }
    if (error != NULL && error_size != 0) error[0] = '\0';
    return RAFT_CONFIG_OK;
}

static void sort_peers(raft_cluster_config_t *config)
{
    for (size_t i = 1; i < config->peer_count; ++i) {
        raft_peer_config_t peer = config->peers[i];
        size_t j = i;
        while (j > 0 && config->peers[j - 1].id > peer.id) {
            config->peers[j] = config->peers[j - 1];
            --j;
        }
        config->peers[j] = peer;
    }
}

raft_config_status_t raft_config_load_file(const char *path,
    raft_cluster_config_t *out, char *error, size_t error_size)
{
    if (path == NULL || *path == '\0' || out == NULL) {
        set_error(error, error_size, "configuration path and destination are required");
        return RAFT_CONFIG_INVALID_ARGUMENT;
    }
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        set_error(error, error_size, "cannot open '%s': %s", path, strerror(errno));
        return RAFT_CONFIG_IO_ERROR;
    }
    raft_cluster_config_t tmp = {0};
    char line[CONFIG_LINE_SIZE];
    unsigned long line_no = 0;
    int seen_node_id = 0;
    raft_config_status_t status = RAFT_CONFIG_OK;
    while (fgets(line, sizeof(line), fp) != NULL) {
        ++line_no;
        size_t len = strlen(line);
        if (len == sizeof(line) - 1 && line[len - 1] != '\n') {
            set_error(error, error_size, "line %lu: line is too long", line_no);
            status = RAFT_CONFIG_PARSE_ERROR;
            break;
        }
        char *record = trim(line);
        if (*record == '\0' || *record == '#') continue;
        char *equals = strchr(record, '=');
        if (equals == NULL) {
            set_error(error, error_size, "line %lu: expected key=value", line_no);
            status = RAFT_CONFIG_PARSE_ERROR;
            break;
        }
        *equals++ = '\0';
        char *key = trim(record);
        char *value = trim(equals);
        if (strcmp(key, "node_id") == 0) {
            uint32_t id;
            if (seen_node_id || !parse_u32(value, &id)) {
                set_error(error, error_size, "line %lu: duplicate or invalid node_id", line_no);
                status = RAFT_CONFIG_PARSE_ERROR;
                break;
            }
            tmp.node_id = id;
            seen_node_id = 1;
        } else if (strcmp(key, "peer") == 0) {
            if (tmp.peer_count == RAFT_CLUSTER_SIZE ||
                !parse_peer(value, &tmp.peers[tmp.peer_count])) {
                set_error(error, error_size, "line %lu: invalid peer; use peer=id,IPv4,port (max 3)", line_no);
                status = RAFT_CONFIG_PARSE_ERROR;
                break;
            }
            ++tmp.peer_count;
        } else {
            set_error(error, error_size, "line %lu: unknown key '%s'", line_no, key);
            status = RAFT_CONFIG_PARSE_ERROR;
            break;
        }
    }
    if (status == RAFT_CONFIG_OK && ferror(fp)) {
        set_error(error, error_size, "error reading '%s'", path);
        status = RAFT_CONFIG_IO_ERROR;
    }
    if (fclose(fp) != 0 && status == RAFT_CONFIG_OK) {
        set_error(error, error_size, "error closing '%s'", path);
        status = RAFT_CONFIG_IO_ERROR;
    }
    if (status != RAFT_CONFIG_OK) return status;
    if (!seen_node_id) {
        set_error(error, error_size, "missing node_id");
        return RAFT_CONFIG_VALIDATION_ERROR;
    }
    status = raft_config_validate(&tmp, error, error_size);
    if (status != RAFT_CONFIG_OK) return status;
    sort_peers(&tmp);
    *out = tmp; /* Commit only after complete validation. */
    if (error != NULL && error_size != 0) error[0] = '\0';
    return RAFT_CONFIG_OK;
}

const char *raft_config_status_name(raft_config_status_t status)
{
    switch (status) {
    case RAFT_CONFIG_OK: return "ok";
    case RAFT_CONFIG_INVALID_ARGUMENT: return "invalid argument";
    case RAFT_CONFIG_IO_ERROR: return "I/O error";
    case RAFT_CONFIG_PARSE_ERROR: return "parse error";
    case RAFT_CONFIG_VALIDATION_ERROR: return "validation error";
    default: return "unknown error";
    }
}
