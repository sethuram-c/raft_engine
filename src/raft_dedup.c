#include "raft_dedup.h"
#include <string.h>

static void write32(uint8_t *dst, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) dst[i] = (uint8_t)(v >> (24u - 8u*i));
}
static uint32_t read32(const uint8_t *p)
{
    uint32_t v = 0;
    for (unsigned i=0; i<4; ++i) v = (v<<8)|p[i];
    return v;
}
static void write64(uint8_t *dst, uint64_t v)
{
    for (unsigned i=0; i<8; ++i) dst[i] = (uint8_t)(v >> (56u-8u*i));
}
static uint64_t read64(const uint8_t *p)
{
    uint64_t v=0;
    for (unsigned i=0; i<8; ++i) v=(v<<8)|p[i];
    return v;
}
int raft_dedup_wrap(uint64_t client_id, uint64_t request_id,
                    const uint8_t *command, size_t length,
                    uint8_t *output, size_t capacity, size_t *out_length)
{
    if (!client_id || !request_id || !output || !out_length ||
        !raft_client_write_valid_command(command,length) ||
        capacity < RAFT_DEDUP_PREFIX + length) return -1;
    memcpy(output,RAFT_DEDUP_LOG_MAGIC,4);
    write64(output+4,client_id);
    write64(output+12,request_id);
    memcpy(output+RAFT_DEDUP_PREFIX,command,length);
    *out_length=RAFT_DEDUP_PREFIX+length;
    return 0;
}
int raft_dedup_parse(const uint8_t *data, size_t length, raft_dedup_entry_t *out)
{
    if (!data || length < 4 || memcmp(data,RAFT_DEDUP_LOG_MAGIC,4)!=0) return 0;
    if (!out || length <= RAFT_DEDUP_PREFIX) return -1;
    raft_dedup_entry_t tmp = {read64(data+4),read64(data+12),
                              data+RAFT_DEDUP_PREFIX,length-RAFT_DEDUP_PREFIX};
    if (!tmp.client_id || !tmp.request_id ||
        !raft_client_write_valid_command(tmp.command,tmp.command_len)) return -1;
    *out=tmp;
    return 1;
}
int raft_dedup_encode_request(uint32_t timeout_ms,uint64_t client_id,
                              uint64_t request_id,const uint8_t *command,
                              size_t length,uint8_t *out,size_t capacity,
                              size_t *out_length)
{
    if (timeout_ms<RAFT_CLIENT_WRITE_MIN_TIMEOUT_MS ||
        timeout_ms>RAFT_CLIENT_WRITE_MAX_TIMEOUT_MS ||
        !client_id || !request_id || !out || !out_length ||
        !raft_client_write_valid_command(command,length) ||
        capacity < RAFT_DEDUP_WIRE_PREFIX+length) return -1;
    write32(out,timeout_ms);
    write64(out+4,client_id);
    write64(out+12,request_id);
    memcpy(out+RAFT_DEDUP_WIRE_PREFIX,command,length);
    *out_length=RAFT_DEDUP_WIRE_PREFIX+length;
    return 0;
}
int raft_dedup_decode_request(const uint8_t *data,size_t length,
                              uint32_t *timeout_ms,raft_dedup_entry_t *out)
{
    if (!data || !timeout_ms || !out || length <= RAFT_DEDUP_WIRE_PREFIX ||
        length > RAFT_DEDUP_WIRE_PREFIX + RAFT_CLIENT_WRITE_MAX_COMMAND) return -1;
    uint32_t timeout=read32(data);
    raft_dedup_entry_t tmp={read64(data+4),read64(data+12),
                            data+RAFT_DEDUP_WIRE_PREFIX,length-RAFT_DEDUP_WIRE_PREFIX};
    if (timeout<RAFT_CLIENT_WRITE_MIN_TIMEOUT_MS ||
        timeout>RAFT_CLIENT_WRITE_MAX_TIMEOUT_MS ||
        !tmp.client_id || !tmp.request_id ||
        !raft_client_write_valid_command(tmp.command,tmp.command_len)) return -1;
    *timeout_ms=timeout; *out=tmp;
    return 0;
}
