#include "raft_dedup.h"
#include "raft_snapshot.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int checks=0;
#define CHECK(x) do {++checks; if (!(x)) {fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)

static void append_id(raft_node_t *n, uint64_t client, uint64_t req,
                      const uint8_t *command, size_t len)
{
    uint8_t bytes[RAFT_DEDUP_PREFIX+RAFT_CLIENT_WRITE_MAX_COMMAND];
    size_t nbytes=0;
    CHECK(raft_dedup_wrap(client,req,command,len,bytes,sizeof(bytes),&nbytes)==0);
    CHECK(raft_log_append(&n->log,n->current_term,bytes,nbytes)==RAFT_OK);
}

static void test_codec(void)
{
    uint8_t buffer[1100]; size_t length=0;
    const uint8_t cmd[]={'S','E','T',' ','a',' ','X',0,'Y'};
    CHECK(raft_dedup_wrap(44,8,cmd,sizeof(cmd),buffer,sizeof(buffer),&length)==0);
    CHECK(length==sizeof(cmd)+20);
    CHECK(memcmp(buffer,"RID1",4)==0);
    raft_dedup_entry_t entry;
    CHECK(raft_dedup_parse(buffer,length,&entry)==1);
    CHECK(entry.client_id==44 && entry.request_id==8);
    CHECK(entry.command_len==sizeof(cmd));
    CHECK(memcmp(entry.command,cmd,sizeof(cmd))==0);
    CHECK(raft_dedup_parse((const uint8_t *)"SET a 1",7,&entry)==0);
    CHECK(raft_dedup_parse(buffer,5,&entry)==-1);
    CHECK(raft_dedup_wrap(0,1,cmd,sizeof(cmd),buffer,sizeof(buffer),&length)==-1);
    CHECK(raft_dedup_wrap(1,0,cmd,sizeof(cmd),buffer,sizeof(buffer),&length)==-1);
    CHECK(raft_dedup_wrap(1,1,cmd,sizeof(cmd),buffer,3,&length)==-1);
    CHECK(raft_dedup_encode_request(800,44,8,cmd,sizeof(cmd),buffer,
                                    sizeof(buffer),&length)==0);
    uint32_t timeout=0;
    CHECK(raft_dedup_decode_request(buffer,length,&timeout,&entry)==0);
    CHECK(timeout==800 && entry.client_id==44 && entry.request_id==8);
    CHECK(entry.command_len==sizeof(cmd) && memcmp(entry.command,cmd,sizeof(cmd))==0);
    buffer[3]=1;
    CHECK(raft_dedup_decode_request(buffer,length,&timeout,&entry)==0);
    buffer[3]=0;
    CHECK(raft_dedup_decode_request(buffer,12,&timeout,&entry)==-1);
    CHECK(raft_dedup_encode_request(10,44,8,cmd,sizeof(cmd),buffer,
                                    sizeof(buffer),&length)==-1);
}
static void test_application_and_snapshot(void)
{
    raft_node_t node,fresh;
    raft_kv_store_t store,restored;
    raft_snapshot_t snapshot,decoded;
    CHECK(raft_node_init(&node,1)==RAFT_OK);
    node.current_term=3;
    raft_kv_init(&store);
    append_id(&node,42,1,(const uint8_t *)"SET counter first",17);
    append_id(&node,42,1,(const uint8_t *)"SET counter first",17);
    append_id(&node,42,1,(const uint8_t *)"SET counter WRONG",17);
    append_id(&node,42,2,(const uint8_t *)"SET counter second",18);
    append_id(&node,11,1,(const uint8_t *)"SET another two",15);
    node.commit_index=5;
    raft_index_t applied=0;
    CHECK(raft_apply_committed(&node,&store,&applied)==RAFT_OK);
    CHECK(applied==5 && node.last_applied==5);
    CHECK(store.session_count==2);
    CHECK(store.executed_commands==3 && store.ignored_commands==2);
    CHECK(store.sessions[0].request_id==2 && store.sessions[0].index==4);
    CHECK(store.sessions[1].client_id==11);
    const uint8_t *val=NULL; size_t vlen=0;
    CHECK(raft_kv_get(&store,(const uint8_t *)"counter",7,&val,&vlen)==1);
    CHECK(vlen==6 && memcmp(val,"second",6)==0);
    raft_snapshot_init(&snapshot);raft_snapshot_init(&decoded);
    CHECK(raft_snapshot_capture(&node,&store,&snapshot)==RAFT_OK);
    uint8_t *bytes=NULL; size_t length=0;
    CHECK(raft_snapshot_encode(&snapshot,&bytes,&length)==RAFT_OK);
    CHECK(length>50 && bytes[11]==2); /* snapshot v2 */
    CHECK(raft_snapshot_decode(bytes,length,&decoded)==RAFT_OK);
    CHECK(decoded.state.session_count==2);
    CHECK(decoded.state.sessions[0].command_len==18);
    CHECK(memcmp(decoded.state.sessions[0].command,"SET counter second",18)==0);
    bytes[length-8]^=0x10;
    CHECK(raft_snapshot_decode(bytes,length,&decoded)!=RAFT_OK);
    bytes[length-8]^=0x10;
    CHECK(raft_node_init(&fresh,2)==RAFT_OK);
    fresh.current_term=4;raft_kv_init(&restored);
    CHECK(raft_snapshot_restore_fresh(&decoded,&fresh,&restored)==RAFT_OK);
    CHECK(restored.session_count==2 && fresh.log.base_index==5);
    CHECK(restored.sessions[0].request_id==2);
    CHECK(raft_kv_get(&restored,(const uint8_t *)"counter",7,&val,&vlen)==1);
    CHECK(vlen==6 && memcmp(val,"second",6)==0);
    append_id(&fresh,42,2,(const uint8_t *)"SET counter second",18);
    append_id(&fresh,42,1,(const uint8_t *)"SET counter first",17);
    append_id(&fresh,42,3,(const uint8_t *)"DEL counter",11);
    fresh.commit_index=8;
    CHECK(raft_apply_committed(&fresh,&restored,&applied)==RAFT_OK);
    CHECK(applied==3 && fresh.last_applied==8);
    CHECK(restored.sessions[0].request_id==3);
    CHECK(restored.executed_commands==4 && restored.ignored_commands==4);
    CHECK(raft_kv_get(&restored,(const uint8_t *)"counter",7,&val,&vlen)==0);
    free(bytes);
    raft_snapshot_destroy(&snapshot);raft_snapshot_destroy(&decoded);
    raft_node_destroy(&node);raft_node_destroy(&fresh);
    raft_kv_destroy(&store);raft_kv_destroy(&restored);
}

int main(void)
{
    test_codec();
    test_application_and_snapshot();
    printf("PASS: Phase 23 request IDs, exact command matching, snapshot recovery (%d checks)\n",checks);
    return 0;
}
