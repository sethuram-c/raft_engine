#define _POSIX_C_SOURCE 200809L
#include "raft_auth.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define RAFT_AUTH_MAX_FILE 4096u

void raft_auth_clear_keys(raft_auth_keys_t *keys)
{
    if (keys) OPENSSL_cleanse(keys, sizeof(*keys));
}
void raft_auth_clear_secret(uint8_t key[RAFT_AUTH_KEY_BYTES])
{
    if (key) OPENSSL_cleanse(key, RAFT_AUTH_KEY_BYTES);
}

/* Fail closed for insecure credentials; don't follow symlinks or accept a
 * file with a different owner, accessible to group/other, or oversized. */
static int read_private_file(const char *path, char *out, size_t capacity)
{
    if (!path || !*path || !out || capacity < 2) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    int ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
             st.st_uid == geteuid() && (st.st_mode & 0077) == 0 &&
             st.st_size >= 1 && (uint64_t)st.st_size < capacity;
    size_t used = 0;
    while (ok && used < capacity) {
        ssize_t n = read(fd, out + used, capacity - used);
        if (n == 0) break;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { ok = 0; break; }
        used += (size_t)n;
    }
    (void)close(fd);
    if (!ok || used == capacity || used == 0 || memchr(out, '\0', used)) {
        OPENSSL_cleanse(out, capacity);
        return -1;
    }
    out[used] = '\0';
    return 0;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int parse_hex(const char *text, size_t length, uint8_t *out)
{
    if (length != 2 * RAFT_AUTH_KEY_BYTES) return -1;
    for (size_t i = 0; i < RAFT_AUTH_KEY_BYTES; ++i) {
        int hi = hex_nibble(text[2*i]);
        int lo = hex_nibble(text[2*i+1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

int raft_auth_read_secret(const char *path, uint8_t key[RAFT_AUTH_KEY_BYTES])
{
    if (!key) return -1;
    char bytes[RAFT_AUTH_MAX_FILE+1] = {0};
    int result = -1;
    if (read_private_file(path,bytes,sizeof(bytes)) == 0) {
        size_t length = strlen(bytes);
        if (length && bytes[length-1]=='\n') --length;
        if (parse_hex(bytes,length,key)==0) result=0;
    }
    if (result!=0) raft_auth_clear_secret(key);
    OPENSSL_cleanse(bytes,sizeof(bytes));
    return result;
}

int raft_auth_read_keyring(const char *path, raft_auth_keys_t *out)
{
    if (!out) return -1;
    raft_auth_keys_t keys = {0};
    char bytes[RAFT_AUTH_MAX_FILE+1] = {0};
    int result = -1;
    if (read_private_file(path,bytes,sizeof(bytes))!=0) goto done;
    char *save=NULL;
    for (char *line=strtok_r(bytes,"\n",&save);line;
         line=strtok_r(NULL,"\n",&save)) {
        if (*line=='#') continue;
        if (keys.count >= RAFT_AUTH_MAX_CLIENTS) goto done;
        char *space=strchr(line,' ');
        if (!space || space==line || space[1]=='\0' || strchr(space+1,' ')) goto done;
        *space='\0';
        for (char *p=line;*p;++p) if (*p<'0'||*p>'9') goto done;
        errno=0;
        char *end=NULL;
        unsigned long long id=strtoull(line,&end,10);
        if (errno || !id || !end || *end!='\0') goto done;
        for (size_t i=0;i<keys.count;i++)
            if (keys.clients[i].client_id==(uint64_t)id) goto done;
        raft_auth_client_t *client=&keys.clients[keys.count];
        if (parse_hex(space+1,strlen(space+1),client->key)!=0) goto done;
        client->client_id=(uint64_t)id;
        keys.count++;
    }
    if (!keys.count) goto done;
    *out=keys;
    result=0;
done:
    raft_auth_clear_keys(&keys);
    OPENSSL_cleanse(bytes,sizeof(bytes));
    return result;
}

/* OpenSSL is used instead of hand-rolled cryptography. The MAC covers the
 * exact on-wire request bytes plus a protocol-specific context string. */
static int tag(const uint8_t key[RAFT_AUTH_KEY_BYTES],
               const uint8_t *data, size_t length,
               uint8_t output[RAFT_AUTH_TAG_BYTES])
{
    const char domain[] = RAFT_AUTH_DOMAIN;
    uint8_t input[sizeof(domain)-1+RAFT_DEDUP_WIRE_PREFIX+RAFT_CLIENT_WRITE_MAX_COMMAND];
    if (length > RAFT_DEDUP_WIRE_PREFIX+RAFT_CLIENT_WRITE_MAX_COMMAND) return -1;
    memcpy(input,domain,sizeof(domain)-1);
    memcpy(input+sizeof(domain)-1,data,length);
    unsigned int output_len=0;
    int ok=HMAC(EVP_sha256(),key,RAFT_AUTH_KEY_BYTES,input,
                sizeof(domain)-1+length,output,&output_len)!=NULL &&
           output_len==RAFT_AUTH_TAG_BYTES;
    OPENSSL_cleanse(input,sizeof(input));
    return ok ? 0 : -1;
}

int raft_auth_sign(uint32_t timeout_ms, uint64_t client_id,
                   uint64_t request_id, const uint8_t *command, size_t command_len,
                   const uint8_t key[RAFT_AUTH_KEY_BYTES],
                   uint8_t *output, size_t capacity, size_t *output_len)
{
    if (!key || !output || !output_len ||
        capacity<RAFT_AUTH_TAG_BYTES ||
        raft_dedup_encode_request(timeout_ms,client_id,request_id,command,
                                  command_len,output,capacity-RAFT_AUTH_TAG_BYTES,
                                  output_len)!=0) return -1;
    if (tag(key,output,*output_len,output+*output_len)!=0) return -1;
    *output_len+=RAFT_AUTH_TAG_BYTES;
    return 0;
}

int raft_auth_verify(const raft_auth_keys_t *keys, const uint8_t *payload,
                     size_t payload_len, uint32_t *timeout_ms,
                     raft_dedup_entry_t *request)
{
    if (!keys || !payload || !timeout_ms || !request ||
        payload_len<=RAFT_AUTH_TAG_BYTES) return -1;
    size_t body_len=payload_len-RAFT_AUTH_TAG_BYTES;
    uint32_t parsed_timeout;
    raft_dedup_entry_t parsed_request;
    if (raft_dedup_decode_request(payload,body_len,&parsed_timeout,&parsed_request)!=0)
        return -1;
    const uint8_t *key=NULL;
    for (size_t i=0;i<keys->count;i++)
        if (keys->clients[i].client_id==parsed_request.client_id)
            key=keys->clients[i].key;
    if (!key) return -1;
    uint8_t computed[RAFT_AUTH_TAG_BYTES];
    int result=tag(key,payload,body_len,computed)==0 &&
               CRYPTO_memcmp(computed,payload+body_len,RAFT_AUTH_TAG_BYTES)==0 ? 0 : -1;
    OPENSSL_cleanse(computed,sizeof(computed));
    if (result==0) {
        *timeout_ms=parsed_timeout;
        *request=parsed_request;
    }
    return result;
}
