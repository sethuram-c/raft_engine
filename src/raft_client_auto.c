#define _POSIX_C_SOURCE 200809L
#include "raft_client_auto.h"
#include "raft_dedup.h"
#include "raft_tcp.h"
#include "raft_wire.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static uint64_t big_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}
static uint32_t big_u32(const uint8_t *p)
{
    return ((uint32_t)p[0]<<24) | ((uint32_t)p[1]<<16) |
           ((uint32_t)p[2]<<8) | p[3];
}

int raft_auto_decode_status(const uint8_t *data, size_t length,
                            raft_node_id_t expected_id,
                            raft_auto_peer_state_t *out)
{
    if (!data || !out || length != 17 || !expected_id ||
        big_u32(data) != expected_id || data[12] > 2 ||
        (data[12] == 2 && big_u64(data+4) == 0)) return -1;
    raft_auto_peer_state_t tmp = {expected_id,big_u64(data+4),data[12],1};
    *out = tmp;
    return 0;
}

raft_node_id_t raft_auto_choose_leader(const raft_auto_peer_state_t *states,
                                        size_t count)
{
    if (!states || !count || count > RAFT_CLUSTER_SIZE) return 0;
    uint64_t max_term = 0;
    for (size_t i = 0; i < count; ++i)
        if (states[i].reachable && states[i].term > max_term)
            max_term = states[i].term;
    if (max_term == 0) return 0;
    raft_node_id_t leader = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!states[i].reachable || states[i].term != max_term ||
            states[i].role != 2) continue;
        if (!states[i].id || leader != 0) return 0;
        leader = states[i].id;
    }
    return leader;
}

int raft_auto_write_decision(raft_client_write_status_t status)
{
    if (status == RAFT_CLIENT_WRITE_COMMITTED) return 0;
    if (status == RAFT_CLIENT_WRITE_REJECTED) return 4;
    return -1;
}

static int monotonic_ms(uint64_t *out)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC,&ts)!=0) return -1;
    *out=(uint64_t)ts.tv_sec*1000u+(uint64_t)ts.tv_nsec/1000000u;
    return 0;
}

static unsigned remaining_ms(uint64_t deadline)
{
    uint64_t now=0;
    if (monotonic_ms(&now)!=0 || now>=deadline) return 0;
    uint64_t remaining=deadline-now;
    return remaining>UINT32_MAX ? UINT32_MAX : (unsigned)remaining;
}

static unsigned smaller(unsigned a, unsigned b) {return a<b?a:b;}

static int connect_bounded(const raft_peer_config_t *peer, unsigned limit)
{
    char error[256];
    unsigned connect_ms=smaller(limit,250u);
    if (!connect_ms) return -1;
    int fd=raft_tcp_connect(peer,(int)connect_ms,error,sizeof(error));
    if (fd<0) return -1;
    unsigned io_ms=smaller(limit,500u);
    struct timeval tv={.tv_sec=(time_t)(io_ms/1000u),
                       .tv_usec=(suseconds_t)((io_ms%1000u)*1000u)};
    if (setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv))!=0 ||
        setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv))!=0) {
        (void)close(fd);return -1;
    }
    return fd;
}

static int status_peer(const raft_peer_config_t *peer, unsigned remaining,
                       raft_auto_peer_state_t *out)
{
    int fd=connect_bounded(peer,remaining);
    if (fd<0) return -1;
    char error[256];
    raft_wire_status_t status=raft_wire_send(fd,RAFT_WIRE_STATUS_REQUEST,
                                              NULL,0,error,sizeof(error));
    uint8_t payload[17];
    size_t length=0;
    raft_wire_type_t type=0;
    if (status==RAFT_WIRE_OK)
        status=raft_wire_recv(fd,&type,payload,sizeof(payload),&length,
                              error,sizeof(error));
    (void)close(fd);
    if (status!=RAFT_WIRE_OK || type!=RAFT_WIRE_STATUS_RESPONSE) return -1;
    return raft_auto_decode_status(payload,length,peer->id,out);
}

/* Returns -1 if transport/response uncertain, otherwise decoded response. */
static int write_peer(const raft_peer_config_t *peer, unsigned remaining,
                      const uint8_t *request, size_t request_len,
                      raft_client_write_response_t *out)
{
    int fd=connect_bounded(peer,remaining);
    if (fd<0) return -1;
    /* A single attempt is deliberately short; the overall budget bounds
     * repeated attempts, and an unknown outcome is safe to retry by ID. */
    char error[256];
    raft_wire_status_t status=raft_wire_send(fd,RAFT_WIRE_CLIENT_WRITE_ID,
                              request,request_len,error,sizeof(error));
    raft_wire_type_t type=0;
    uint8_t bytes[RAFT_CLIENT_WRITE_RESPONSE_BYTES];
    size_t length=0;
    if (status==RAFT_WIRE_OK)
        status=raft_wire_recv(fd,&type,bytes,sizeof(bytes),&length,
                              error,sizeof(error));
    (void)close(fd);
    if (status!=RAFT_WIRE_OK || type!=RAFT_WIRE_CLIENT_WRITE_ID_RESPONSE ||
        raft_client_write_decode_response(bytes,length,out)!=0) return -1;
    return 0;
}

static void pause_bounded(unsigned remaining)
{
    unsigned duration=smaller(RAFT_AUTO_RETRY_DELAY_MS,remaining);
    struct timespec req={.tv_sec=0,.tv_nsec=(long)duration*1000000L};
    while (nanosleep(&req,&req)!=0 && errno==EINTR) { }
}

int raft_client_auto_write(const raft_cluster_config_t *config,
                            uint32_t overall_timeout_ms,
                            uint64_t client_id, uint64_t request_id,
                            const uint8_t *command, size_t command_length)
{
    char error[256];
    if (!config || raft_config_validate(config,error,sizeof(error))!=RAFT_CONFIG_OK ||
        overall_timeout_ms<RAFT_AUTO_MIN_BUDGET_MS ||
        overall_timeout_ms>RAFT_AUTO_MAX_BUDGET_MS) return 1;
    uint8_t request[RAFT_DEDUP_WIRE_PREFIX+RAFT_CLIENT_WRITE_MAX_COMMAND];
    size_t request_len=0;
    /* Encode ONCE, then reuse the identical bytes in every attempt. */
    if (raft_dedup_encode_request(450,client_id,request_id,command,command_length,
                                  request,sizeof(request),&request_len)!=0) return 4;
    uint64_t start=0;
    if (monotonic_ms(&start)!=0 || UINT64_MAX-start<overall_timeout_ms) return 1;
    uint64_t deadline=start+overall_timeout_ms;
    unsigned attempts=0,scans=0;
    raft_node_id_t previously_tried=0;
    while (remaining_ms(deadline)>0) {
        raft_auto_peer_state_t observations[RAFT_CLUSTER_SIZE]={{0}};
        for (size_t i=0;i<config->peer_count;i++) {
            observations[i].id=config->peers[i].id;
            unsigned rem=remaining_ms(deadline);
            if (!rem) break;
            (void)status_peer(&config->peers[i],rem,&observations[i]);
        }
        ++scans;
        raft_node_id_t leader=raft_auto_choose_leader(observations,config->peer_count);
        if (leader && remaining_ms(deadline)>0) {
            const raft_peer_config_t *peer=raft_config_peer_for_id(config,leader);
            if (peer) {
                if (previously_tried!=leader) {
                    uint64_t discovered_term=0;
                    for (size_t i=0;i<config->peer_count;i++)
                        if (observations[i].id==leader)
                            discovered_term=observations[i].term;
                    printf("Discovered leader: node=%u term=%" PRIu64 "\n",
                           leader,discovered_term);
                }
                previously_tried=leader;
                raft_client_write_response_t reply={0};
                ++attempts;
                if (write_peer(peer,remaining_ms(deadline),request,request_len,&reply)==0) {
                    int decision=raft_auto_write_decision(reply.status);
                    if (decision==0 || decision==4) {
                        printf("Automatic write: status=%s leader=%u term=%" PRIu64
                               " index=%" PRIu64 " attempts=%u scans=%u client=%" PRIu64
                               " request=%" PRIu64 "\n",
                               raft_client_write_status_name(reply.status),leader,
                               (uint64_t)reply.term,(uint64_t)reply.index,
                               attempts,scans,client_id,request_id);
                        return decision;
                    }
                }
            }
        }
        unsigned rem=remaining_ms(deadline);
        if (rem) pause_bounded(rem);
    }
    printf("Automatic write: status=UNKNOWN attempts=%u scans=%u client=%" PRIu64
           " request=%" PRIu64 "\n",attempts,scans,client_id,request_id);
    puts("INDETERMINATE: retry with exactly the same client ID, request ID, and command.");
    return 3;
}
