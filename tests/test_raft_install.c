#include "raft_install.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks=0;
#define CHECK(x) do { ++checks; if (!(x)) { \
  fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } \
} while(0)

static void setup(raft_node_t *node, raft_node_id_t id, raft_term_t term)
{
    CHECK(raft_node_init(node,id)==RAFT_OK);
    node->current_term=term;
    CHECK(raft_node_start_follower(node,1000,id*7+1)==RAFT_OK);
}

int main(void)
{
    raft_node_t sender, receiver;
    raft_kv_store_t leader_kv, follower_kv;
    raft_snapshot_t snapshot;
    raft_snapshot_init(&snapshot);
    raft_kv_init(&leader_kv); raft_kv_init(&follower_kv);
    setup(&sender,1,3);
    const uint8_t a[]={'S','E','T',' ','a','l','p','h','a',' ','A',0,'B'};
    CHECK(raft_log_append(&sender.log,3,a,sizeof(a))==RAFT_OK);
    CHECK(raft_log_append(&sender.log,3,"SET beta second",15)==RAFT_OK);
    sender.commit_index=2;
    raft_index_t applied=0;
    CHECK(raft_apply_committed(&sender,&leader_kv,&applied)==RAFT_OK && applied==2);
    CHECK(raft_snapshot_capture(&sender,&leader_kv,&snapshot)==RAFT_OK);
    uint8_t *bytes=NULL;
    size_t length=0;
    CHECK(raft_snapshot_encode(&snapshot,&bytes,&length)==RAFT_OK);
    CHECK(length>52 && length<RAFT_SNAPSHOT_MAX_BYTES);

    raft_install_chunk_t chunk={3,1,snapshot.last_index,snapshot.last_term,
                                0,(uint32_t)length,0,bytes,17};
    uint8_t wire[RAFT_WIRE_MAX_PAYLOAD]; size_t wire_length=0;
    CHECK(raft_install_encode_chunk(&chunk,wire,sizeof(wire),&wire_length)==RAFT_OK);
    CHECK(wire_length==57);
    raft_install_chunk_t decoded;
    CHECK(raft_install_decode_chunk(wire,wire_length,&decoded)==RAFT_OK);
    CHECK(decoded.offset==0 && decoded.total==length && decoded.data_len==17);
    CHECK(memcmp(decoded.data,bytes,17)==0);
    wire[37]=1;
    CHECK(raft_install_decode_chunk(wire,wire_length,&decoded)!=RAFT_OK);
    wire[37]=0; wire[38]=1;
    CHECK(raft_install_decode_chunk(wire,wire_length,&decoded)!=RAFT_OK);
    wire[38]=0;
    CHECK(raft_install_decode_chunk(wire,wire_length,&decoded)==RAFT_OK);
    chunk.done=1;
    CHECK(raft_install_encode_chunk(&chunk,wire,sizeof(wire),&wire_length)!=RAFT_OK);
    chunk.done=0;
    chunk.total=0;
    CHECK(raft_install_encode_chunk(&chunk,wire,sizeof(wire),&wire_length)!=RAFT_OK);
    chunk.total=(uint32_t)length;

    raft_install_response_t reply={3,1,17}, response;
    uint8_t reply_wire[RAFT_INSTALL_RESPONSE_BYTES];
    CHECK(raft_install_encode_response(&reply,reply_wire)==RAFT_OK);
    CHECK(raft_install_decode_response(reply_wire,sizeof(reply_wire),&response)==RAFT_OK);
    CHECK(response.term==3 && response.accepted==1 && response.next_offset==17);
    reply_wire[8]=2;
    CHECK(raft_install_decode_response(reply_wire,sizeof(reply_wire),&response)!=RAFT_OK);

    setup(&receiver,2,2);
    raft_install_receiver_t inflight;
    raft_install_receiver_init(&inflight);
    uint8_t *complete=NULL;
    size_t completed_length=0;
    /* Missing first chunk does not allocate or mutate log state. */
    chunk.offset=17; chunk.data=bytes+17;
    CHECK(raft_install_receive(&inflight,&receiver,&follower_kv,&chunk,
         1100,&reply,&complete,&completed_length)==RAFT_OK);
    CHECK(!reply.accepted && reply.next_offset==0 && !inflight.bytes);
    CHECK(receiver.current_term==3 && receiver.role==RAFT_FOLLOWER);
    chunk.offset=0; chunk.data=bytes;
    CHECK(raft_install_receive(&inflight,&receiver,&follower_kv,&chunk,
         1110,&reply,&complete,&completed_length)==RAFT_OK);
    CHECK(reply.accepted && reply.next_offset==17);
    CHECK(receiver.last_applied==0 && receiver.log.base_index==0);
    /* Out-of-order data reports required offset without corrupting assembly. */
    chunk.offset=18; chunk.data=bytes+18;
    CHECK(raft_install_receive(&inflight,&receiver,&follower_kv,&chunk,
         1120,&reply,&complete,&completed_length)==RAFT_OK);
    CHECK(!reply.accepted && reply.next_offset==17);
    chunk.offset=0; chunk.data=bytes;
    CHECK(raft_install_receive(&inflight,&receiver,&follower_kv,&chunk,
         1130,&reply,&complete,&completed_length)==RAFT_OK);
    CHECK(!reply.accepted && reply.next_offset==17);

    for(size_t offset=17;offset<length;) {
        size_t count=length-offset>11 ? 11 : length-offset;
        chunk.offset=(uint32_t)offset; chunk.data=bytes+offset;
        chunk.data_len=count; chunk.done=(offset+count==length);
        CHECK(raft_install_encode_chunk(&chunk,wire,sizeof(wire),&wire_length)==RAFT_OK);
        CHECK(raft_install_decode_chunk(wire,wire_length,&decoded)==RAFT_OK);
        CHECK(raft_install_receive(&inflight,&receiver,&follower_kv,&decoded,
              1200+offset,&reply,&complete,&completed_length)==RAFT_OK);
        CHECK(reply.accepted && reply.next_offset==offset+count);
        offset+=count;
    }
    CHECK(complete && completed_length==length);
    CHECK(memcmp(complete,bytes,length)==0);
    free(complete);
    CHECK(receiver.log.base_index==2 && receiver.log.base_term==3);
    CHECK(receiver.last_applied==2 && receiver.commit_index==2);
    CHECK(receiver.log.count==0 && raft_log_last_index(&receiver.log)==2);
    CHECK(raft_node_is_valid(&receiver));
    const uint8_t *value=NULL; size_t n=0;
    CHECK(raft_kv_get(&follower_kv,(const uint8_t *)"alpha",5,&value,&n)==1);
    CHECK(n==3 && memcmp(value,"A\0B",3)==0);
    CHECK(follower_kv.executed_commands==2 && follower_kv.ignored_commands==0);
    /* A repeated complete chunk after application is safely idempotent. */
    CHECK(raft_install_receive(&inflight,&receiver,&follower_kv,&decoded,
          1400,&reply,&complete,&completed_length)==RAFT_OK);
    CHECK(reply.accepted && reply.next_offset==length && !complete);
    CHECK(raft_log_append(&receiver.log,3,"SET gamma works",15)==RAFT_OK);
    receiver.commit_index=3;
    CHECK(raft_apply_committed(&receiver,&follower_kv,&applied)==RAFT_OK);
    CHECK(receiver.last_applied==3 && follower_kv.executed_commands==3);

    /* Corrupted bytes are detected at the end and never installed. */
    raft_node_t corrupt;
    raft_kv_store_t kv_corrupt;
    raft_kv_init(&kv_corrupt);
    setup(&corrupt,3,3);
    raft_install_receiver_t bad;
    raft_install_receiver_init(&bad);
    uint8_t *corrupted=malloc(length);
    CHECK(corrupted!=NULL);
    memcpy(corrupted,bytes,length); corrupted[15]^=7;
    chunk=(raft_install_chunk_t){3,1,2,3,0,(uint32_t)length,1,corrupted,length};
    CHECK(raft_install_receive(&bad,&corrupt,&kv_corrupt,&chunk,1300,
          &reply,&complete,&completed_length)==RAFT_OK);
    CHECK(!reply.accepted && !bad.bytes && corrupt.last_applied==0);
    free(corrupted);
    /* A stale-term sender cannot overwrite follower state. */
    chunk=(raft_install_chunk_t){1,1,2,3,0,(uint32_t)length,1,bytes,length};
    CHECK(raft_install_receive(&bad,&corrupt,&kv_corrupt,&chunk,1400,
          &reply,&complete,&completed_length)!=RAFT_OK); /* term < last_term invalid */
    chunk.term=2;
    CHECK(raft_install_receive(&bad,&corrupt,&kv_corrupt,&chunk,1400,
          &reply,&complete,&completed_length)!=RAFT_OK);
    chunk.term=3;
    CHECK(raft_install_receive(&bad,&corrupt,&kv_corrupt,&chunk,1400,
          &reply,&complete,&completed_length)==RAFT_OK);
    CHECK(reply.accepted && corrupt.last_applied==2);
    free(complete);
    /* A mismatching local committed suffix must never be destroyed. */
    raft_node_t committed;
    raft_kv_store_t kv_committed;
    raft_kv_init(&kv_committed);
    setup(&committed,4,3);
    CHECK(raft_log_append(&committed.log,1,"SET x a",7)==RAFT_OK);
    CHECK(raft_log_append(&committed.log,2,"SET y b",7)==RAFT_OK);
    CHECK(raft_log_append(&committed.log,3,"SET z c",7)==RAFT_OK);
    committed.commit_index=3;
    raft_install_receiver_t refused;
    raft_install_receiver_init(&refused);
    chunk=(raft_install_chunk_t){3,1,2,3,0,(uint32_t)length,1,bytes,length};
    CHECK(raft_install_receive(&refused,&committed,&kv_committed,&chunk,1500,
          &reply,&complete,&completed_length)==RAFT_OK);
    CHECK(!reply.accepted && reply.next_offset==0);
    CHECK(committed.commit_index==3 && committed.log.base_index==0);
    CHECK(committed.log.count==3 && committed.last_applied==0);
    CHECK(!complete);
    raft_install_receiver_destroy(&refused);
    raft_node_destroy(&committed);
    raft_kv_destroy(&kv_committed);
    raft_install_receiver_destroy(&bad);
    raft_install_receiver_destroy(&inflight);
    raft_snapshot_destroy(&snapshot);
    raft_kv_destroy(&leader_kv); raft_kv_destroy(&follower_kv);
    raft_kv_destroy(&kv_corrupt);
    raft_node_destroy(&sender); raft_node_destroy(&receiver); raft_node_destroy(&corrupt);
    free(bytes);
    printf("PASS: %d Phase 20 chunk codec, partial transfer, CRC, installation and replay assertions\n",checks);
    return 0;
}
