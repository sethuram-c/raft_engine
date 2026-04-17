#define _POSIX_C_SOURCE 200809L
#include "raft_wal.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL WAL %s:%d %s\n",__FILE__,__LINE__,#x);exit(1); } } while(0)
static off_t filesize(const char *name)
{
    struct stat st;CHECK(stat(name,&st)==0);return st.st_size;
}
static void newnode(raft_node_t *n,raft_node_id_t id)
{
    CHECK(raft_node_init(n,id)==RAFT_OK);
    CHECK(raft_node_start_follower(n,10000,123)==RAFT_OK);
}
int main(void)
{
    char dir[]="/tmp/raft17-unit-XXXXXX";
    CHECK(mkdtemp(dir)!=NULL);
    char error[256]={0}, path[256];
    snprintf(path,sizeof(path),"%s/node-2.wal",dir);
    raft_wal_t wal;
    CHECK(raft_wal_open(&wal,dir,2,error,sizeof(error))==0);
    CHECK(filesize(path)==20);
    raft_node_t node;
    newnode(&node,2);
    int found=-1;
    CHECK(raft_wal_load(&wal,&node,&found,error,sizeof(error))==0 && !found);
    CHECK(raft_wal_save(&wal,&node,error,sizeof(error))==0);
    CHECK(filesize(path)==20);
    printf("PASS WAL: empty bootstrap and no-op save\n");

    node.current_term=3;
    node.voted_for=2;
    CHECK(raft_wal_save(&wal,&node,error,sizeof(error))==0);
    off_t meta_size=filesize(path);
    CHECK(meta_size==20+8+36);
    uint8_t binary[]={'S','E','T',' ','k',' ','A',0,'B'};
    CHECK(raft_log_append(&node.log,3,binary,sizeof(binary))==RAFT_OK);
    CHECK(raft_wal_save(&wal,&node,error,sizeof(error))==0);
    CHECK(filesize(path)-meta_size==8+36+12+(off_t)sizeof(binary));
    CHECK(raft_log_append(&node.log,3,"DEL k",5)==RAFT_OK);
    node.commit_index=1;
    CHECK(raft_wal_save(&wal,&node,error,sizeof(error))==0);
    off_t stable_size=filesize(path);
    CHECK(raft_wal_save(&wal,&node,error,sizeof(error))==0);
    CHECK(filesize(path)==stable_size);
    printf("PASS WAL: metadata-only record, binary suffix, commit, no-op\n");

    raft_wal_t duplicate;
    CHECK(raft_wal_open(&duplicate,dir,2,error,sizeof(error))!=0);
    raft_wal_close(&wal);
    raft_wal_t reopen;
    CHECK(raft_wal_open(&reopen,dir,2,error,sizeof(error))==0);
    raft_node_t recovered;
    newnode(&recovered,2);
    CHECK(raft_wal_load(&reopen,&recovered,&found,error,sizeof(error))==0 && found);
    CHECK(recovered.current_term==3 && recovered.voted_for==2);
    CHECK(recovered.commit_index==1 && recovered.last_applied==0);
    CHECK(raft_log_last_index(&recovered.log)==2);
    CHECK(raft_log_at(&recovered.log,1)->command_len==sizeof(binary));
    CHECK(memcmp(raft_log_at(&recovered.log,1)->command,binary,sizeof(binary))==0);
    printf("PASS WAL: exclusive lock, restart, byte-for-byte replay\n");

    /* Rewrite uncommitted suffix; previously committed index 1 is immutable. */
    CHECK(raft_log_truncate_from(&recovered.log,2)==RAFT_OK);
    recovered.current_term=4;
    recovered.voted_for=RAFT_NO_VOTE;
    CHECK(raft_log_append(&recovered.log,4,"SET z 1",7)==RAFT_OK);
    CHECK(raft_wal_save(&reopen,&recovered,error,sizeof(error))==0);
    CHECK(raft_log_truncate_from(&recovered.log,1)==RAFT_OK);
    CHECK(raft_wal_save(&reopen,&recovered,error,sizeof(error))!=0);
    CHECK(raft_log_append(&recovered.log,4,binary,sizeof(binary))==RAFT_OK);
    /* Restore consistency by loading a fresh node, never reuse a failed write. */
    raft_wal_close(&reopen);
    CHECK(raft_wal_open(&reopen,dir,2,error,sizeof(error))==0);
    raft_node_t replayed;
    newnode(&replayed,2);
    CHECK(raft_wal_load(&reopen,&replayed,&found,error,sizeof(error))==0);
    CHECK(replayed.commit_index==1 && replayed.current_term==4);
    CHECK(raft_log_last_index(&replayed.log)==2);
    CHECK(raft_log_at(&replayed.log,2)->term==4);
    CHECK(memcmp(raft_log_at(&replayed.log,2)->command,"SET z 1",7)==0);
    printf("PASS WAL: conflict suffix replacement; committed-prefix rollback rejected\n");

    /* Simulate SIGKILL in the middle of the final record: incomplete tail only. */
    off_t before=filesize(path);
    int fd=open(path,O_WRONLY|O_APPEND);
    CHECK(fd>=0);
    uint8_t fragment[]={0,0,0,36,0,0,0,0,1,2,3};
    CHECK(write(fd,fragment,sizeof(fragment))==(ssize_t)sizeof(fragment));
    CHECK(close(fd)==0);
    raft_wal_close(&reopen);
    CHECK(raft_wal_open(&reopen,dir,2,error,sizeof(error))==0);
    raft_node_t tail;
    newnode(&tail,2);
    CHECK(raft_wal_load(&reopen,&tail,&found,error,sizeof(error))==0);
    CHECK(filesize(path)==before);
    tail.commit_index=2;
    CHECK(raft_wal_save(&reopen,&tail,error,sizeof(error))==0);
    raft_wal_close(&reopen);
    CHECK(raft_wal_open(&reopen,dir,2,error,sizeof(error))==0);
    raft_node_t tail_replay;
    newnode(&tail_replay,2);
    CHECK(raft_wal_load(&reopen,&tail_replay,&found,error,sizeof(error))==0);
    CHECK(tail_replay.commit_index==2);
    printf("PASS WAL: incomplete-tail truncation, fsync, subsequent append/replay\n");

    /* A COMPLETE record with a broken CRC is never silently discarded. */
    raft_wal_close(&reopen);
    fd=open(path,O_RDWR);CHECK(fd>=0);
    uint8_t x;CHECK(pread(fd,&x,1,20+8+8)==1);
    x^=0x5a;
    CHECK(pwrite(fd,&x,1,20+8+8)==1);
    CHECK(close(fd)==0);
    CHECK(raft_wal_open(&reopen,dir,2,error,sizeof(error))==0);
    raft_node_t bad;
    newnode(&bad,2);
    CHECK(raft_wal_load(&reopen,&bad,&found,error,sizeof(error))!=0);
    CHECK(strstr(error,"corrupt")!=NULL);
    CHECK(bad.current_term==0 && raft_log_last_index(&bad.log)==0);
    raft_wal_close(&reopen);
    printf("PASS WAL: complete-record CRC corruption fails closed\n");

    /* Cross-format safety: do not bootstrap Phase16 in an existing WAL dir. */
    raft_storage_t snapshot;
    CHECK(raft_storage_open(&snapshot,dir,2,error,sizeof(error))==0);
    CHECK(raft_storage_load(&snapshot,&bad,&found,error,sizeof(error))!=0);
    raft_storage_close(&snapshot);
    printf("PASS WAL: prevents Phase16 and Phase17 disk histories from diverging\n");

    raft_node_destroy(&node);raft_node_destroy(&recovered);
    raft_node_destroy(&replayed);raft_node_destroy(&tail);
    raft_node_destroy(&tail_replay);raft_node_destroy(&bad);
    CHECK(unlink(path)==0);
    snprintf(path,sizeof(path),"%s/node-2.lock",dir);
    CHECK(unlink(path)==0);
    CHECK(rmdir(dir)==0);
    puts("All Phase 17 WAL unit tests passed.");
    return 0;
}
