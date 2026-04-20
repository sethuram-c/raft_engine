#include "raft_install.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void w16(uint8_t *p, uint16_t x) { p[0]=(uint8_t)(x>>8); p[1]=(uint8_t)x; }
static void w32(uint8_t *p, uint32_t x)
{ for(unsigned i=0;i<4;i++) p[i]=(uint8_t)(x>>(24u-8u*i)); }
static void w64(uint8_t *p, uint64_t x)
{ for(unsigned i=0;i<8;i++) p[i]=(uint8_t)(x>>(56u-8u*i)); }
static uint16_t r16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0]<<8)|p[1]); }
static uint32_t r32(const uint8_t *p)
{ uint32_t x=0; for(unsigned i=0;i<4;i++) x=(x<<8)|p[i]; return x; }
static uint64_t r64(const uint8_t *p)
{ uint64_t x=0; for(unsigned i=0;i<8;i++) x=(x<<8)|p[i]; return x; }

static int valid_chunk(const raft_install_chunk_t *c)
{
    return c && c->term && c->leader_id != RAFT_NO_VOTE &&
           c->last_index && c->last_term && c->last_term <= c->term &&
           c->total >= 52 && c->total <= RAFT_SNAPSHOT_MAX_BYTES &&
           c->data && c->data_len && c->data_len <= RAFT_INSTALL_CHUNK_BYTES &&
           c->offset <= c->total && c->data_len <= c->total - c->offset &&
           (c->done == 0 || c->done == 1) &&
           (c->done == 1) == (c->offset + c->data_len == c->total);
}

void raft_install_receiver_init(raft_install_receiver_t *r)
{ if (r) memset(r,0,sizeof(*r)); }
void raft_install_receiver_destroy(raft_install_receiver_t *r)
{ if (r) { free(r->bytes); raft_install_receiver_init(r); } }

raft_status_t raft_install_encode_chunk(const raft_install_chunk_t *c,
                                        uint8_t *out,size_t capacity,size_t *length)
{
    if (!valid_chunk(c) || !out || !length ||
        capacity < RAFT_INSTALL_HEADER_BYTES + c->data_len)
        return RAFT_ERR_INVALID_ARGUMENT;
    w64(out,c->term); w32(out+8,c->leader_id); w64(out+12,c->last_index);
    w64(out+20,c->last_term); w32(out+28,c->offset); w32(out+32,c->total);
    out[36]=(uint8_t)c->done; out[37]=0; w16(out+38,(uint16_t)c->data_len);
    memcpy(out+RAFT_INSTALL_HEADER_BYTES,c->data,c->data_len);
    *length=RAFT_INSTALL_HEADER_BYTES+c->data_len;
    return RAFT_OK;
}
raft_status_t raft_install_decode_chunk(const uint8_t *p,size_t length,
                                        raft_install_chunk_t *out)
{
    if (!p || !out || length<=RAFT_INSTALL_HEADER_BYTES ||
        length>RAFT_WIRE_MAX_PAYLOAD || p[37]!=0 ||
        r16(p+38)!=length-RAFT_INSTALL_HEADER_BYTES) return RAFT_ERR_INVALID_ARGUMENT;
    raft_install_chunk_t c={r64(p),r32(p+8),r64(p+12),r64(p+20),
        r32(p+28),r32(p+32),p[36],p+RAFT_INSTALL_HEADER_BYTES,
        length-RAFT_INSTALL_HEADER_BYTES};
    if (!valid_chunk(&c)) return RAFT_ERR_INVALID_ARGUMENT;
    *out=c; return RAFT_OK;
}
raft_status_t raft_install_encode_response(const raft_install_response_t *r,
                                           uint8_t out[RAFT_INSTALL_RESPONSE_BYTES])
{
    if (!r || !out || (r->accepted!=0 && r->accepted!=1))
        return RAFT_ERR_INVALID_ARGUMENT;
    w64(out,r->term); out[8]=(uint8_t)r->accepted; w32(out+9,r->next_offset);
    return RAFT_OK;
}
raft_status_t raft_install_decode_response(const uint8_t *p,size_t length,
                                           raft_install_response_t *out)
{
    if (!p || !out || length!=RAFT_INSTALL_RESPONSE_BYTES || p[8]>1)
        return RAFT_ERR_INVALID_ARGUMENT;
    *out=(raft_install_response_t){r64(p),p[8],r32(p+9)};
    return RAFT_OK;
}

raft_status_t raft_install_receive(raft_install_receiver_t *r,raft_node_t *node,
                                   raft_kv_store_t *store,const raft_install_chunk_t *c,
                                   uint64_t now_ms,raft_install_response_t *reply,
                                   uint8_t **completed,size_t *completed_length)
{
    if (!r || !node || !store || !reply || !completed || !completed_length ||
        !valid_chunk(c) || c->leader_id==node->id ||
        !raft_node_is_valid(node) || !node->election_timer.armed)
        return RAFT_ERR_INVALID_ARGUMENT;
    *completed=NULL; *completed_length=0;
    *reply=(raft_install_response_t){node->current_term,0,0};
    if (c->term<node->current_term) return RAFT_OK;
    /* A valid leader message refreshes the follower timeout. */
    if (raft_election_timer_reset(&node->election_timer,now_ms)!=RAFT_OK)
        return RAFT_ERR_INVALID_ARGUMENT;
    if (c->term>node->current_term) {
        node->current_term=c->term; node->voted_for=RAFT_NO_VOTE;
        raft_install_receiver_destroy(r);
    }
    node->role=RAFT_FOLLOWER;
    reply->term=node->current_term;
    /* Already applied this (or a newer) snapshot. Safe idempotent ack. */
    if (c->last_index<=node->last_applied) {
        reply->accepted=1; reply->next_offset=c->total; return RAFT_OK;
    }
    if (r->bytes && (r->term!=c->term || r->leader_id!=c->leader_id ||
        r->last_index!=c->last_index || r->last_term!=c->last_term ||
        r->total!=c->total)) raft_install_receiver_destroy(r);
    if (!r->bytes) {
        if (c->offset!=0) return RAFT_OK;
        r->bytes=malloc(c->total);
        if (!r->bytes) return RAFT_ERR_NO_MEMORY;
        r->total=c->total; r->term=c->term; r->leader_id=c->leader_id;
        r->last_index=c->last_index; r->last_term=c->last_term;
    }
    if (c->offset!=r->received) {
        reply->next_offset=r->received; return RAFT_OK;
    }
    memcpy(r->bytes+c->offset,c->data,c->data_len);
    r->received+=(uint32_t)c->data_len;
    reply->accepted=1; reply->next_offset=r->received;
    if (!c->done) return RAFT_OK;

    raft_snapshot_t decoded;
    raft_snapshot_init(&decoded);
    if (raft_snapshot_decode(r->bytes,r->total,&decoded)!=RAFT_OK ||
        decoded.last_index!=c->last_index || decoded.last_term!=c->last_term) {
        raft_snapshot_destroy(&decoded);
        raft_install_receiver_destroy(r);
        reply->accepted=0; reply->next_offset=0;
        return RAFT_OK;
    }
    /* A follower must never destroy a committed, disagreeing suffix. */
    int matching=raft_log_matches(&node->log,c->last_index,c->last_term);
    if (!matching && node->commit_index>c->last_index) {
        raft_snapshot_destroy(&decoded);
        raft_install_receiver_destroy(r);
        reply->accepted=0; reply->next_offset=0;
        return RAFT_OK;
    }
    if (matching) {
        if (raft_log_compact_prefix(&node->log,c->last_index,c->last_term)!=RAFT_OK) {
            raft_snapshot_destroy(&decoded);
            return RAFT_ERR_INVALID_ARGUMENT;
        }
    } else {
        raft_log_destroy(&node->log);
        node->log.base_index=c->last_index;
        node->log.base_term=c->last_term;
    }
    raft_kv_destroy(store);
    *store=decoded.state; raft_kv_init(&decoded.state);
    raft_snapshot_destroy(&decoded);
    if (node->commit_index<c->last_index) node->commit_index=c->last_index;
    node->last_applied=c->last_index;
    *completed=r->bytes; *completed_length=r->total;
    r->bytes=NULL;
    raft_install_receiver_destroy(r);
    return RAFT_OK;
}
