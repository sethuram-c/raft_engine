#define _POSIX_C_SOURCE 200809L
#include "raft_tls.h"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Phase 27: the mutex protects the context pointer and its configuration.
 * Every connection takes a reference before leaving the mutex. SSL objects
 * retain their own context reference, so hot rotation never frees a context
 * that an active handshake or connection still uses. */
static pthread_mutex_t context_lock = PTHREAD_MUTEX_INITIALIZER;
static SSL_CTX *context;
static raft_cluster_config_t members;
static char cert_path[1024], key_path[1024], ca_path[1024];
static raft_node_id_t reload_node;
static _Thread_local SSL *active;
static _Thread_local int active_fd = -1;
static _Thread_local raft_node_id_t authenticated_node;
static _Thread_local int authenticated_client;

#define RAFT_TLS_INBOUND_HANDSHAKE_MS 1200
#define RAFT_TLS_OUTBOUND_HANDSHAKE_MS 450

static void set_error(char *error, size_t cap, const char *message)
{
    if (error && cap) (void)snprintf(error, cap, "%s", message);
}

static int exact_identity(X509 *cert, const char *name)
{
    return cert && X509_check_host(cert, name, 0,
        X509_CHECK_FLAG_NEVER_CHECK_SUBJECT | X509_CHECK_FLAG_NO_WILDCARDS,
        NULL) == 1;
}

static int certificate_is_node(SSL_CTX *ctx, raft_node_id_t id)
{
    char identity[64];
    (void)snprintf(identity, sizeof(identity), "raft-node-%u", id);
    return exact_identity(SSL_CTX_get0_certificate(ctx), identity);
}

static int key_is_private(const char *key)
{
    struct stat st;
    /* No symbolic links, world/group permissions or another user's key.
     * The PEM pathname still needs a trusted directory to avoid TOCTOU. */
    return lstat(key, &st) == 0 && S_ISREG(st.st_mode) &&
           st.st_uid == geteuid() && (st.st_mode & 077) == 0;
}

static SSL_CTX *load_context(const char *cert, const char *key, const char *ca,
                             char *error, size_t cap)
{
    if (!key_is_private(key)) {
        set_error(error, cap, "TLS private key must be an owner-only regular file");
        return NULL;
    }
    SSL_CTX *ctx = SSL_CTX_new(TLS_method());
    if (!ctx) { set_error(error, cap, "TLS context creation failed"); return NULL; }
    if (SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION) != 1 ||
        SSL_CTX_load_verify_locations(ctx, ca, NULL) != 1 ||
        SSL_CTX_use_certificate_file(ctx, cert, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1) {
        set_error(error, cap, "TLS certificate, key, or CA configuration failed");
        SSL_CTX_free(ctx);
        return NULL;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
    SSL_CTX_set_verify_depth(ctx, 3);
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION |
                             SSL_OP_NO_TICKET);
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
    return ctx;
}

int raft_tls_enabled(void)
{
    (void)pthread_mutex_lock(&context_lock);
    int enabled = context != NULL;
    (void)pthread_mutex_unlock(&context_lock);
    return enabled;
}

int raft_tls_validate_local_node(raft_node_id_t node_id)
{
    (void)pthread_mutex_lock(&context_lock);
    int valid = context == NULL ||
        (raft_config_peer_for_id(&members, node_id) &&
         certificate_is_node(context, node_id));
    (void)pthread_mutex_unlock(&context_lock);
    return valid ? 0 : -1;
}

void raft_tls_detach(void)
{
    if (active) SSL_free(active);
    active = NULL;
    active_fd = -1;
    authenticated_node = 0;
    authenticated_client = 0;
}

raft_node_id_t raft_tls_remote_node(void) { return authenticated_node; }
int raft_tls_remote_client(void) { return authenticated_client; }

int raft_tls_configure(const char *cert, const char *key, const char *ca,
                       const raft_cluster_config_t *cluster,
                       char *error, size_t cap)
{
    if (!cert || !key || !ca || !cluster ||
        strlen(cert) >= sizeof(cert_path) ||
        strlen(key) >= sizeof(key_path) ||
        strlen(ca) >= sizeof(ca_path) ||
        raft_config_validate(cluster, error, cap) != RAFT_CONFIG_OK) {
        set_error(error, cap, "TLS configuration invalid");
        return -1;
    }
    SSL_CTX *ctx = load_context(cert, key, ca, error, cap);
    if (!ctx) return -1;
    (void)pthread_mutex_lock(&context_lock);
    if (context != NULL) {
        (void)pthread_mutex_unlock(&context_lock);
        SSL_CTX_free(ctx);
        set_error(error, cap, "TLS is already configured");
        return -1;
    }
    /* OpenSSL's socket BIO does not use Linux MSG_NOSIGNAL. A peer closing
     * during a TLS write must return EPIPE, not kill the entire Raft node. */
    struct sigaction ignore_pipe;
    memset(&ignore_pipe, 0, sizeof(ignore_pipe));
    ignore_pipe.sa_handler = SIG_IGN;
    (void)sigemptyset(&ignore_pipe.sa_mask);
    if (sigaction(SIGPIPE, &ignore_pipe, NULL) != 0) {
        (void)pthread_mutex_unlock(&context_lock);
        SSL_CTX_free(ctx);
        set_error(error, cap, "cannot ignore SIGPIPE for TLS sockets");
        return -1;
    }
    members = *cluster;
    (void)snprintf(cert_path, sizeof(cert_path), "%s", cert);
    (void)snprintf(key_path, sizeof(key_path), "%s", key);
    (void)snprintf(ca_path, sizeof(ca_path), "%s", ca);
    context = ctx;
    (void)pthread_mutex_unlock(&context_lock);
    if (error && cap) error[0] = '\0';
    return 0;
}

int raft_tls_enable_reload(raft_node_id_t node_id)
{
    (void)pthread_mutex_lock(&context_lock);
    int valid = context && raft_config_peer_for_id(&members, node_id) &&
                certificate_is_node(context, node_id);
    if (valid) reload_node = node_id;
    (void)pthread_mutex_unlock(&context_lock);
    return valid ? 0 : -1;
}

int raft_tls_reload_enabled(void)
{
    (void)pthread_mutex_lock(&context_lock);
    int valid = context != NULL && reload_node != 0;
    (void)pthread_mutex_unlock(&context_lock);
    return valid;
}

int raft_tls_reload(char *error, size_t cap)
{
    (void)pthread_mutex_lock(&context_lock);
    if (!context || !reload_node) {
        (void)pthread_mutex_unlock(&context_lock);
        set_error(error, cap, "TLS reload is not enabled for this node");
        return -1;
    }
    /* The paths/config are immutable after configuration. Only the context
     * pointer changes on reload, and it changes under context_lock. */
    raft_node_id_t node = reload_node;
    (void)pthread_mutex_unlock(&context_lock);

    SSL_CTX *replacement = load_context(cert_path, key_path, ca_path, error, cap);
    if (!replacement) return -1;
    if (!certificate_is_node(replacement, node)) {
        SSL_CTX_free(replacement);
        set_error(error, cap, "TLS rotated certificate SAN does not match local node ID");
        return -1;
    }
    (void)pthread_mutex_lock(&context_lock);
    SSL_CTX *previous = context;
    context = replacement;
    (void)pthread_mutex_unlock(&context_lock);
    SSL_CTX_free(previous);
    if (error && cap) error[0] = '\0';
    return 0;
}

static int identity_from_cert(SSL *ssl, raft_node_id_t expected,
                              char *error, size_t cap)
{
    X509 *cert = SSL_get1_peer_certificate(ssl);
    if (!cert || SSL_get_verify_result(ssl) != X509_V_OK) {
        if (cert) X509_free(cert);
        set_error(error, cap, "TLS peer certificate is missing or untrusted");
        return -1;
    }
    int ok = 0;
    raft_node_id_t node = 0;
    int client = 0;
    if (expected) {
        char name[64];
        (void)snprintf(name, sizeof(name), "raft-node-%u", expected);
        if (raft_config_peer_for_id(&members, expected) && exact_identity(cert, name)) {
            node = expected;
            ok = 1;
        }
    } else {
        for (size_t i = 0; i < members.peer_count; ++i) {
            char name[64];
            raft_node_id_t id = members.peers[i].id;
            (void)snprintf(name, sizeof(name), "raft-node-%u", id);
            if (exact_identity(cert, name)) { node = id; ok = 1; break; }
        }
        if (!ok && exact_identity(cert, "raft-client")) { client = 1; ok = 1; }
    }
    X509_free(cert);
    if (!ok) {
        set_error(error, cap, "TLS peer SAN identity is not authorized");
        return -1;
    }
    authenticated_node = node;
    authenticated_client = client;
    return 0;
}

static int64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* One overall deadline, even if a malicious peer sends a byte at a time.
 * The fd is nonblocking for the handshake only; all subsequent framed I/O
 * continues using the established socket's usual timeouts. */
static int handshake(SSL *ssl, int fd, int outbound, char *error, size_t cap)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        set_error(error, cap, "TLS socket flags unavailable");
        return -1;
    }
    int64_t started = monotonic_ms();
    int64_t deadline = started < 0 ? -1 : started +
        (outbound ? RAFT_TLS_OUTBOUND_HANDSHAKE_MS : RAFT_TLS_INBOUND_HANDSHAKE_MS);
    int success = 0;
    while (deadline >= 0) {
        int status = outbound ? SSL_connect(ssl) : SSL_accept(ssl);
        if (status == 1) { success = 1; break; }
        int ssl_error = SSL_get_error(ssl, status);
        if (ssl_error != SSL_ERROR_WANT_READ && ssl_error != SSL_ERROR_WANT_WRITE)
            break;
        int64_t now = monotonic_ms();
        if (now < 0 || now >= deadline) break;
        int remaining = (int)(deadline - now);
        struct pollfd wanted = {.fd = fd,
            .events = ssl_error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT,
            .revents = 0};
        int ready = poll(&wanted, 1, remaining);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || (wanted.revents & (POLLERR | POLLNVAL | POLLHUP))) break;
    }
    if (fcntl(fd, F_SETFL, flags) < 0) success = 0;
    if (!success) set_error(error, cap, "TLS 1.3 handshake failed or exceeded deadline");
    return success ? 0 : -1;
}

static int attach(int fd, raft_node_id_t expected, int outbound,
                  char *error, size_t cap)
{
    (void)pthread_mutex_lock(&context_lock);
    SSL_CTX *ctx = context;
    if (ctx && SSL_CTX_up_ref(ctx) != 1) ctx = NULL;
    (void)pthread_mutex_unlock(&context_lock);
    if (!ctx) { set_error(error, cap, "TLS context unavailable"); return -1; }
    raft_tls_detach();
    SSL *ssl = SSL_new(ctx);
    SSL_CTX_free(ctx); /* SSL_new owns a distinct context reference. */
    if (!ssl || SSL_set_fd(ssl, fd) != 1) {
        if (ssl) SSL_free(ssl);
        set_error(error, cap, "TLS socket allocation failed");
        return -1;
    }
    if (handshake(ssl, fd, outbound, error, cap) != 0 ||
        identity_from_cert(ssl, expected, error, cap) != 0) {
        SSL_free(ssl);
        return -1;
    }
    active = ssl;
    active_fd = fd;
    return 0;
}

int raft_tls_attach_outbound(int fd, raft_node_id_t expected,
                             char *error, size_t cap)
{
    if (!raft_tls_enabled()) return 0; /* Phase 1-25 plaintext modes */
    return expected ? attach(fd, expected, 1, error, cap) : -1;
}
int raft_tls_attach_inbound(int fd, char *error, size_t cap)
{
    if (!raft_tls_enabled()) return 0; /* Phase 1-25 plaintext modes */
    return attach(fd, 0, 0, error, cap);
}

ssize_t raft_tls_read(int fd, void *buf, size_t size)
{
    if (!active || active_fd != fd) { errno = ENOTCONN; return -1; }
    if (size > INT_MAX) size = INT_MAX;
    int got = SSL_read(active, buf, (int)size);
    if (got > 0) return got;
    int err = SSL_get_error(active, got);
    if (err == SSL_ERROR_ZERO_RETURN) return 0;
    errno = (err == SSL_ERROR_SYSCALL && errno == EINTR) ? EINTR : EIO;
    return -1;
}

ssize_t raft_tls_write(int fd, const void *buf, size_t size)
{
    if (!active || active_fd != fd) { errno = ENOTCONN; return -1; }
    if (size > INT_MAX) size = INT_MAX;
    int got = SSL_write(active, buf, (int)size);
    if (got > 0) return got;
    int err = SSL_get_error(active, got);
    errno = (err == SSL_ERROR_SYSCALL && errno == EINTR) ? EINTR : EIO;
    return -1;
}
