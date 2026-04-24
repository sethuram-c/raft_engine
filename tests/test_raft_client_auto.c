#include "raft_client_auto.h"
#include "raft_dedup.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put_u32(uint8_t *p,uint32_t v)
{
    for(int i=0;i<4;i++) p[i]=(uint8_t)(v>>(24-8*i));
}
static void put_u64(uint8_t *p,uint64_t v)
{
    for(int i=0;i<8;i++) p[i]=(uint8_t)(v>>(56-8*i));
}

int main(void)
{
    unsigned checks=0;
#define CHECK(x) do { if (!(x)) {fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;} ++checks; } while(0)
    uint8_t packet[17]={0};
    put_u32(packet,3);put_u64(packet+4,11);packet[12]=2;
    put_u32(packet+13,1);
    raft_auto_peer_state_t state={77,77,77,77};
    CHECK(raft_auto_decode_status(packet,17,3,&state)==0);
    CHECK(state.reachable && state.id==3 && state.term==11 && state.role==2);
    raft_auto_peer_state_t old=state;
    CHECK(raft_auto_decode_status(packet,16,3,&state)!=0);
    CHECK(memcmp(&state,&old,sizeof(state))==0);
    CHECK(raft_auto_decode_status(packet,17,2,&state)!=0);
    CHECK(memcmp(&state,&old,sizeof(state))==0);
    packet[12]=3;
    CHECK(raft_auto_decode_status(packet,17,3,&state)!=0);
    CHECK(memcmp(&state,&old,sizeof(state))==0);
    CHECK(raft_auto_decode_status(NULL,17,3,&state)!=0);
    CHECK(raft_auto_decode_status(packet,17,3,NULL)!=0);
    packet[12]=2; put_u64(packet+4,0);
    CHECK(raft_auto_decode_status(packet,17,3,&state)!=0);
    put_u64(packet+4,11);
    raft_auto_peer_state_t three[]={
        {1,11,2,1},{2,11,0,1},{3,11,0,0}
    };
    CHECK(raft_auto_choose_leader(three,3)==1);
    three[0].term=0; three[1].term=0;
    CHECK(raft_auto_choose_leader(three,3)==0);
    three[0].term=11;three[1].term=11;
    three[2].reachable=1; three[2].role=2;
    CHECK(raft_auto_choose_leader(three,3)==0); /* two leaders in term */
    three[0].role=0;three[2].role=2;
    CHECK(raft_auto_choose_leader(three,3)==3);
    three[1].term=12;three[1].role=1;
    CHECK(raft_auto_choose_leader(three,3)==0); /* higher term candidate */
    three[1].term=11;
    CHECK(raft_auto_choose_leader(three,3)==3);
    three[2].reachable=0;
    CHECK(raft_auto_choose_leader(three,3)==0);
    CHECK(raft_auto_choose_leader(NULL,3)==0);
    CHECK(raft_auto_choose_leader(three,0)==0);
    CHECK(raft_auto_choose_leader(three,RAFT_CLUSTER_SIZE+1)==0);
    CHECK(raft_auto_write_decision(RAFT_CLIENT_WRITE_COMMITTED)==0);
    CHECK(raft_auto_write_decision(RAFT_CLIENT_WRITE_REJECTED)==4);
    CHECK(raft_auto_write_decision(RAFT_CLIENT_WRITE_NOT_LEADER)==-1);
    CHECK(raft_auto_write_decision(RAFT_CLIENT_WRITE_TIMEOUT)==-1);
    CHECK(raft_auto_write_decision(RAFT_CLIENT_WRITE_UNKNOWN)==-1);
    CHECK(raft_auto_write_decision(RAFT_CLIENT_WRITE_PENDING)==-1);
    CHECK(raft_auto_write_decision((raft_client_write_status_t)99)==-1);
    uint8_t buf[RAFT_DEDUP_WIRE_PREFIX+128];size_t n=0;
    const uint8_t cmd[]={ 'S','E','T',' ','b',' ','A',0,'B'};
    CHECK(raft_dedup_encode_request(450,999,4,cmd,sizeof(cmd),buf,sizeof(buf),&n)==0);
    uint8_t copy[sizeof(buf)];memcpy(copy,buf,n);
    uint32_t timeout=0;raft_dedup_entry_t parsed={0};
    CHECK(raft_dedup_decode_request(buf,n,&timeout,&parsed)==0);
    CHECK(timeout==450 && parsed.client_id==999 && parsed.request_id==4);
    CHECK(parsed.command_len==sizeof(cmd) && memcmp(parsed.command,cmd,sizeof(cmd))==0);
    CHECK(memcmp(copy,buf,n)==0);
    CHECK(raft_client_auto_write(NULL,1000,1,1,cmd,sizeof(cmd))==1);
    printf("PASS: %u Phase 24 router/identity/decision assertions\n",checks);
    return 0;
}
