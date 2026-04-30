#define _POSIX_C_SOURCE 200809L
#include "raft_fault.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int raft_fault_init(raft_fault_policy_t *policy, raft_node_id_t self_id,
                    const char *directory)
{
    if (!policy || self_id == RAFT_NO_VOTE || !directory || !*directory ||
        strlen(directory) >= sizeof(policy->directory)) return -1;
    struct stat st;
    if (lstat(directory, &st) != 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 077) != 0) return -1;
    memset(policy, 0, sizeof(*policy));
    (void)snprintf(policy->directory, sizeof(policy->directory), "%s", directory);
    policy->self_id = self_id;
    policy->enabled = 1;
    return 0;
}

int raft_fault_blocks(const raft_fault_policy_t *policy, raft_node_id_t peer_id)
{
    if (!policy || !policy->enabled || peer_id == policy->self_id) return 0;
    /* A missing/replaced rule directory must not look like an absent file:
     * revalidate the private directory at every read (fail closed). */
    struct stat directory_stat;
    if (lstat(policy->directory, &directory_stat) != 0 ||
        !S_ISDIR(directory_stat.st_mode) ||
        directory_stat.st_uid != geteuid() ||
        (directory_stat.st_mode & 077) != 0) return 1;
    char path[RAFT_FAULT_DIRECTORY_MAX + 48];
    int length = snprintf(path, sizeof(path), "%s/node-%u.block",
                          policy->directory, policy->self_id);
    if (length < 0 || (size_t)length >= sizeof(path)) return 1;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT ? 0 : 1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 022) != 0 ||
        st.st_size < 0 || st.st_size > (off_t)RAFT_FAULT_RULE_MAX) {
        (void)close(fd);
        return 1;
    }
    char buffer[RAFT_FAULT_RULE_MAX + 1];
    size_t used = 0;
    ssize_t n;
    while ((n = read(fd, buffer + used, sizeof(buffer) - used)) > 0) {
        used += (size_t)n;
        if (used >= sizeof(buffer)) break;
    }
    (void)close(fd);
    if (n < 0 || used > RAFT_FAULT_RULE_MAX) return 1;
    buffer[used] = '\0';
    /* Strict decimal peer IDs separated by ASCII whitespace. Unknown peer IDs
     * are allowed in the policy but never affect another configured peer. */
    size_t pos = 0;
    int blocked = 0;
    while (pos < used) {
        while (pos < used && (buffer[pos] == ' ' || buffer[pos] == '\t' ||
               buffer[pos] == '\r' || buffer[pos] == '\n')) pos++;
        if (pos == used) break;
        if (buffer[pos] < '0' || buffer[pos] > '9') return 1;
        uint64_t value = 0;
        while (pos < used && buffer[pos] >= '0' && buffer[pos] <= '9') {
            unsigned digit = (unsigned)(buffer[pos++] - '0');
            if (value > (UINT32_MAX - digit) / 10u) return 1;
            value = 10u * value + digit;
        }
        if (value == 0) return 1;
        if ((raft_node_id_t)value == peer_id) blocked = 1;
        if (pos < used && buffer[pos] != ' ' && buffer[pos] != '\t' &&
            buffer[pos] != '\r' && buffer[pos] != '\n') return 1;
    }
    return blocked;
}
