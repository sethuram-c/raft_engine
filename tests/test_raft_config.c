#define _POSIX_C_SOURCE 200809L
#include "raft_config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *good =
    "# comments and blank lines are allowed\n"
    "node_id = 2\n\n"
    "peer=3,127.0.0.1,7003\n"
    "peer = 1, 127.0.0.1, 7001\n"
    "peer=2,127.0.0.1,7002\n";

static raft_config_status_t load_text(const char *body,
    raft_cluster_config_t *out, char *error, size_t error_size)
{
    char path[] = "/tmp/raft-config-test-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *fp = fdopen(fd, "w");
    assert(fp != NULL);
    assert(fputs(body, fp) >= 0);
    assert(fclose(fp) == 0);
    raft_config_status_t status = raft_config_load_file(path, out, error, error_size);
    assert(unlink(path) == 0);
    return status;
}

static void test_valid_and_sorted(void)
{
    raft_cluster_config_t cfg = {0};
    char err[256];
    assert(load_text(good, &cfg, err, sizeof(err)) == RAFT_CONFIG_OK);
    assert(cfg.node_id == 2 && cfg.peer_count == 3);
    assert(cfg.peers[0].id == 1 && cfg.peers[1].id == 2 && cfg.peers[2].id == 3);
    assert(cfg.peers[1].port == 7002);
    assert(raft_config_peer_for_id(&cfg, 3) == &cfg.peers[2]);
    assert(raft_config_peer_for_id(&cfg, 5) == NULL);
    assert(err[0] == '\0');
    puts("PASS config: parse whitespace/comments and stable ID order");
}

static void expect_reject(const char *body, const char *reason)
{
    raft_cluster_config_t cfg = {0};
    cfg.node_id = 999; /* Verify failure never overwrites the destination. */
    char err[256] = {0};
    raft_config_status_t status = load_text(body, &cfg, err, sizeof(err));
    if (status == RAFT_CONFIG_OK || cfg.node_id != 999 || err[0] == '\0') {
        fprintf(stderr, "unexpected acceptance: %s\n", reason);
        abort();
    }
    printf("PASS config: reject %s (%s)\n", reason, raft_config_status_name(status));
}

static void test_validation_failures(void)
{
    expect_reject("node_id=1\npeer=1,127.0.0.1,7001\n", "too few peers");
    expect_reject("node_id=4\npeer=1,127.0.0.1,7001\npeer=2,127.0.0.1,7002\npeer=3,127.0.0.1,7003\n", "missing self");
    expect_reject("node_id=1\npeer=1,127.0.0.1,7001\npeer=1,127.0.0.1,7002\npeer=3,127.0.0.1,7003\n", "duplicate IDs");
    expect_reject("node_id=1\npeer=1,127.0.0.1,7001\npeer=2,127.0.0.1,7001\npeer=3,127.0.0.1,7003\n", "duplicate endpoints");
    expect_reject("node_id=1\npeer=1,127.0.0.1,0\npeer=2,127.0.0.1,7002\npeer=3,127.0.0.1,7003\n", "zero port");
    expect_reject("node_id=1\npeer=1,127.0.0.1,65536\npeer=2,127.0.0.1,7002\npeer=3,127.0.0.1,7003\n", "out of range port");
    expect_reject("node_id=1\npeer=1,300.0.0.1,7001\npeer=2,127.0.0.1,7002\npeer=3,127.0.0.1,7003\n", "invalid IPv4");
    expect_reject("node_id=-1\npeer=1,127.0.0.1,7001\npeer=2,127.0.0.1,7002\npeer=3,127.0.0.1,7003\n", "negative ID");
    expect_reject("node_id=4294967296\npeer=1,127.0.0.1,7001\npeer=2,127.0.0.1,7002\npeer=3,127.0.0.1,7003\n", "overflow ID");
    expect_reject("node_id=1\nnode_id=2\npeer=1,127.0.0.1,7001\npeer=2,127.0.0.1,7002\npeer=3,127.0.0.1,7003\n", "duplicate node_id key");
    expect_reject("node_id=1\nwat=1\npeer=1,127.0.0.1,7001\npeer=2,127.0.0.1,7002\npeer=3,127.0.0.1,7003\n", "unknown key");
    expect_reject("node_id=1\npeer=1,127.0.0.1,7001,extra\npeer=2,127.0.0.1,7002\npeer=3,127.0.0.1,7003\n", "extra peer field");
    expect_reject("node_id=1\npeer=1,127.0.0.1,7001\npeer=2,127.0.0.1,7002\npeer=3,127.0.0.1,7003\npeer=4,127.0.0.1,7004\n", "fourth peer");
    expect_reject("node_id=1\npeer=1,127.0.0.1,7001\npeer=2,127.0.0.1,7002\npeer=3,127.0.0.1,7003\nhello\n", "line missing equals sign");
    char longline[700];
    memset(longline, 'x', sizeof(longline) - 1);
    longline[sizeof(longline) - 1] = '\0';
    expect_reject(longline, "oversized line");
}

static void test_crlf_and_io(void)
{
    const char *crlf = "node_id=1\r\npeer=1,127.0.0.1,7001\r\npeer=2,127.0.0.1,7002\r\npeer=3,127.0.0.1,7003\r\n";
    raft_cluster_config_t cfg = {0};
    char err[256];
    assert(load_text(crlf, &cfg, err, sizeof(err)) == RAFT_CONFIG_OK);
    assert(cfg.node_id == 1);
    assert(raft_config_load_file("/does/not/exist/raft.conf", &cfg,
                                 err, sizeof(err)) == RAFT_CONFIG_IO_ERROR);
    assert(raft_config_load_file(NULL, &cfg, err, sizeof(err)) == RAFT_CONFIG_INVALID_ARGUMENT);
    assert(raft_config_load_file("missing.conf", NULL, err, sizeof(err)) == RAFT_CONFIG_INVALID_ARGUMENT);
    puts("PASS config: CRLF, missing file, invalid arguments");
}

static void test_programmatic_validation(void)
{
    raft_cluster_config_t cfg = {0};
    cfg.node_id = 3;
    cfg.peer_count = 3;
    for (size_t i = 0; i < 3; ++i) {
        cfg.peers[i].id = (raft_node_id_t)(i + 1);
        strcpy(cfg.peers[i].ipv4, "127.0.0.1");
        cfg.peers[i].port = (uint16_t)(7001 + i);
    }
    char err[128];
    assert(raft_config_validate(&cfg, err, sizeof(err)) == RAFT_CONFIG_OK);
    cfg.peers[0].port = 0;
    assert(raft_config_validate(&cfg, err, sizeof(err)) == RAFT_CONFIG_VALIDATION_ERROR);
    assert(raft_config_validate(NULL, NULL, 0) == RAFT_CONFIG_INVALID_ARGUMENT);
    puts("PASS config: public in-memory validation");
}

int main(void)
{
    test_valid_and_sorted();
    test_validation_failures();
    test_crlf_and_io();
    test_programmatic_validation();
    puts("Config unit tests passed.");
    return 0;
}
