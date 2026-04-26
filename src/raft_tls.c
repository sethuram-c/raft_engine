#define _POSIX_C_SOURCE 200809L
#include "raft_tls.h"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static SSL_CTX *context;
static raft_cluster_config_t members;
static _Thread_local SSL *active;
static _Thread_local int active_fd = -1;
static _Thread_local raft_node_id_t authenticated_node;
static _Thread_local int authenticated_client;

static void set_error(char *error, size_t cap, const char *message)
{
    if (error && cap) (void)snprintf(error, cap, "%s", message);
}

int raft_tls_enabled(void) { return context != NULL; }

int raft_tls_validate_local_node(raft_node_id_t node_id)
{
    if (!context) return 0;
    X509 *cert = SSL_CTX_get0_certificate(context);
    char name[64];
    (void)snprintf(name, sizeof(name), "raft-node-%u", node_id);
    return cert && raft_config_peer_for_id(&members, node_id) &&
           X509_check_host(cert, name, 0,
              X509_CHECK_FLAG_NEVER_CHECK_SUBJECT | X509_CHECK_FLAG_NO_WILDCARDS,
              NULL) == 1 ? 0 : -1;
}


void raft_tls_detach(void)
{
    if (active) SSL_free(active); /* SSL_set_fd uses a non-owning socket BIO. */
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
    if (context || !cert || !key || !ca || !cluster ||
        raft_config_validate(cluster, error, cap) != RAFT_CONFIG_OK) {
        set_error(error, cap, "TLS configuration invalid or already active");
        return -1;
    }
    SSL_CTX *ctx = SSL_CTX_new(TLS_method());
    if (!ctx) { set_error(error, cap, "TLS context creation failed"); return -1; }
    if (SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION) != 1 ||
        SSL_CTX_load_verify_locations(ctx, ca, NULL) != 1 ||
        SSL_CTX_use_certificate_file(ctx, cert, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1) {
        set_error(error, cap, "TLS certificate, key, or CA configuration failed");
        SSL_CTX_free(ctx);
        return -1;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
    SSL_CTX_set_verify_depth(ctx, 3);
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    members = *cluster;
    context = ctx;
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
        if (raft_config_peer_for_id(&members, expected) &&
            X509_check_host(cert, name, 0, (X509_CHECK_FLAG_NEVER_CHECK_SUBJECT | X509_CHECK_FLAG_NO_WILDCARDS), NULL) == 1) {
            node = expected;
            ok = 1;
        }
    } else {
        for (size_t i = 0; i < members.peer_count; ++i) {
            char name[64];
            raft_node_id_t id = members.peers[i].id;
            (void)snprintf(name, sizeof(name), "raft-node-%u", id);
            if (X509_check_host(cert, name, 0, (X509_CHECK_FLAG_NEVER_CHECK_SUBJECT | X509_CHECK_FLAG_NO_WILDCARDS), NULL) == 1) {
                node = id;
                ok = 1;
                break;
            }
        }
        if (!ok && X509_check_host(cert, "raft-client", 0,
                                  (X509_CHECK_FLAG_NEVER_CHECK_SUBJECT | X509_CHECK_FLAG_NO_WILDCARDS), NULL) == 1) {
            client = 1;
            ok = 1;
        }
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

static int attach(int fd, raft_node_id_t expected, int outbound,
                  char *error, size_t cap)
{
    if (!context) return 0;
    raft_tls_detach();
    SSL *ssl = SSL_new(context);
    if (!ssl || SSL_set_fd(ssl, fd) != 1) {
        if (ssl) SSL_free(ssl);
        set_error(error, cap, "TLS socket allocation failed");
        return -1;
    }
    int result = outbound ? SSL_connect(ssl) : SSL_accept(ssl);
    if (result != 1) {
        set_error(error, cap, "TLS 1.3 handshake failed");
        SSL_free(ssl);
        return -1;
    }
    if (identity_from_cert(ssl, expected, error, cap) != 0) {
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
    return expected ? attach(fd, expected, 1, error, cap) : -1;
}
int raft_tls_attach_inbound(int fd, char *error, size_t cap)
{
    return attach(fd, 0, 0, error, cap);
}

ssize_t raft_tls_read(int fd, void *buf, size_t size)
{
    if (!active || active_fd != fd) {
        errno = ENOTCONN;
        return -1;
    }
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
    if (!active || active_fd != fd) {
        errno = ENOTCONN;
        return -1;
    }
    if (size > INT_MAX) size = INT_MAX;
    int got = SSL_write(active, buf, (int)size);
    if (got > 0) return got;
    int err = SSL_get_error(active, got);
    errno = (err == SSL_ERROR_SYSCALL && errno == EINTR) ? EINTR : EIO;
    return -1;
}
