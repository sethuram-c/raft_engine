#include "raft_health.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; \
} ++checks; } while (0)

int main(void)
{
    raft_health_input_t in = {
        .node_id=2, .role=2, .term=7, .commit_index=6, .last_applied=5,
        .started_ms=1000, .now_ms=5000, .last_ack_ms=4500,
        .last_ack_term=7, .ack_valid=1, .election_enabled=1,
        .storage_healthy=1
    };
    raft_health_sample_t sample;
    CHECK(raft_health_evaluate(&in,&sample)==0);
    CHECK(sample.node_id==2 && sample.role==2 && sample.term==7);
    CHECK(sample.commit_index==6 && sample.last_applied==5);
    CHECK(sample.uptime_ms==4000 && sample.quorum_age_ms==500);
    CHECK(raft_health_write_ready(&sample));
    CHECK(sample.flags==RAFT_HEALTH_FLAGS_MASK);
    uint8_t wire[RAFT_HEALTH_WIRE_BYTES+1];
    memset(wire,0xa5,sizeof(wire));
    CHECK(raft_health_encode(&sample,wire,sizeof(wire))==0);
    CHECK(memcmp(wire,"RHLT",4)==0 && wire[4]==1);
    CHECK(wire[5]==2 && wire[6]==RAFT_HEALTH_FLAGS_MASK && wire[7]==0);
    CHECK(wire[8]==0 && wire[9]==0 && wire[10]==0 && wire[11]==2);
    CHECK(wire[12]==0 && wire[19]==7); /* term is big endian */
    CHECK(wire[sizeof(wire)-1]==0xa5);
    raft_health_sample_t decoded;
    memset(&decoded,0xcc,sizeof(decoded));
    CHECK(raft_health_decode(wire,RAFT_HEALTH_WIRE_BYTES,&decoded)==0);
    CHECK(memcmp(&decoded,&sample,sizeof(sample))==0);
    raft_health_sample_t before=decoded;
    CHECK(raft_health_decode(wire,RAFT_HEALTH_WIRE_BYTES-1,&decoded)!=0);
    CHECK(memcmp(&decoded,&before,sizeof(decoded))==0);
    CHECK(raft_health_decode(NULL,RAFT_HEALTH_WIRE_BYTES,&decoded)!=0);
    CHECK(raft_health_decode(wire,RAFT_HEALTH_WIRE_BYTES,NULL)!=0);
    wire[0]='X';
    CHECK(raft_health_decode(wire,RAFT_HEALTH_WIRE_BYTES,&decoded)!=0);
    wire[0]='R'; wire[4]=2;
    CHECK(raft_health_decode(wire,RAFT_HEALTH_WIRE_BYTES,&decoded)!=0);
    wire[4]=1; wire[7]=1;
    CHECK(raft_health_decode(wire,RAFT_HEALTH_WIRE_BYTES,&decoded)!=0);
    wire[7]=0; wire[6] |= 0x80;
    CHECK(raft_health_decode(wire,RAFT_HEALTH_WIRE_BYTES,&decoded)!=0);
    wire[6]=RAFT_HEALTH_FLAGS_MASK;
    wire[5]=3;
    CHECK(raft_health_decode(wire,RAFT_HEALTH_WIRE_BYTES,&decoded)!=0);
    wire[5]=2;
    CHECK(raft_health_decode(wire,RAFT_HEALTH_WIRE_BYTES,&decoded)==0);
    CHECK(memcmp(&decoded,&before,sizeof(decoded))==0);
    wire[6]=RAFT_HEALTH_WRITE_READY;
    CHECK(raft_health_decode(wire,RAFT_HEALTH_WIRE_BYTES,&decoded)!=0);
    wire[6]=RAFT_HEALTH_ELECTION_ENABLED|RAFT_HEALTH_STORAGE_OK;
    CHECK(raft_health_decode(wire,RAFT_HEALTH_WIRE_BYTES,&decoded)!=0);
    wire[6]=RAFT_HEALTH_FLAGS_MASK;
    wire[28+7]=7; /* applied greater than commit */
    CHECK(raft_health_decode(wire,RAFT_HEALTH_WIRE_BYTES,&decoded)!=0);
    CHECK(memcmp(&decoded,&before,sizeof(decoded))==0);
    CHECK(raft_health_encode(NULL,wire,sizeof(wire))!=0);
    CHECK(raft_health_encode(&sample,NULL,sizeof(wire))!=0);
    CHECK(raft_health_encode(&sample,wire,RAFT_HEALTH_WIRE_BYTES-1)!=0);
    raft_health_sample_t invalid=sample;
    invalid.node_id=0;
    memset(wire,0x91,sizeof(wire));
    CHECK(raft_health_encode(&invalid,wire,sizeof(wire))!=0);
    CHECK(wire[0]==0x91);
    invalid=sample; invalid.role=5;
    CHECK(raft_health_encode(&invalid,wire,sizeof(wire))!=0);
    invalid=sample; invalid.flags=RAFT_HEALTH_WRITE_READY;
    CHECK(raft_health_encode(&invalid,wire,sizeof(wire))!=0);
    invalid=sample; invalid.last_applied=99;
    CHECK(raft_health_encode(&invalid,wire,sizeof(wire))!=0);
    /* Inclusive freshness boundary, invalid after the deadline. */
    in.now_ms=in.last_ack_ms+RAFT_HEALTH_FRESH_MS;
    CHECK(raft_health_evaluate(&in,&sample)==0 && raft_health_write_ready(&sample));
    in.now_ms++;
    CHECK(raft_health_evaluate(&in,&sample)==0 && !raft_health_write_ready(&sample));
    CHECK(sample.quorum_age_ms==UINT64_MAX &&
          !(sample.flags & RAFT_HEALTH_QUORUM_RECENT));
    in.now_ms=5000; in.last_ack_term=6;
    CHECK(raft_health_evaluate(&in,&sample)==0 && !raft_health_write_ready(&sample));
    in.last_ack_term=7; in.ack_valid=0;
    CHECK(raft_health_evaluate(&in,&sample)==0 && !raft_health_write_ready(&sample));
    in.ack_valid=1; in.role=0;
    CHECK(raft_health_evaluate(&in,&sample)==0 && !raft_health_write_ready(&sample));
    CHECK(sample.flags==(RAFT_HEALTH_ELECTION_ENABLED|RAFT_HEALTH_STORAGE_OK));
    in.role=2; in.storage_healthy=0;
    CHECK(raft_health_evaluate(&in,&sample)==0 && !raft_health_write_ready(&sample));
    CHECK(sample.flags==(RAFT_HEALTH_ELECTION_ENABLED|RAFT_HEALTH_QUORUM_RECENT));
    in.storage_healthy=1; in.election_enabled=0;
    CHECK(raft_health_evaluate(&in,&sample)==0 && !raft_health_write_ready(&sample));
    in.election_enabled=1;
    in.now_ms=in.last_ack_ms-1;
    CHECK(raft_health_evaluate(&in,&sample)==0 && !raft_health_write_ready(&sample));
    in.now_ms=5000;
    CHECK(raft_health_evaluate(&in,&sample)==0 && raft_health_write_ready(&sample));
    raft_health_sample_t unchanged=sample;
    in.node_id=0;
    CHECK(raft_health_evaluate(&in,&sample)!=0);
    CHECK(memcmp(&sample,&unchanged,sizeof(sample))==0);
    in.node_id=2; in.role=7;
    CHECK(raft_health_evaluate(&in,&sample)!=0);
    in.role=2; in.commit_index=1;
    CHECK(raft_health_evaluate(&in,&sample)!=0);
    in.commit_index=6; in.now_ms=900;
    CHECK(raft_health_evaluate(&in,&sample)!=0);
    CHECK(raft_health_evaluate(NULL,&sample)!=0);
    CHECK(raft_health_evaluate(&in,NULL)!=0);
    CHECK(!raft_health_write_ready(NULL));
    CHECK(memcmp(&sample,&unchanged,sizeof(sample))==0);
    puts("PASS: Phase 29 health evaluation, freshness, defensive wire codec");
    printf("PASS: %u Phase 29 health assertions\n",checks);
    return 0;
}
