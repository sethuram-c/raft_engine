#define _POSIX_C_SOURCE 200809L
#include "raft_snapwal.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { \
    fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#x);exit(1); \
} } while(0)

static void open_and_load(raft_snapwal_t *w,const char *dir,
                          raft_node_t *n,int *found)
{
    char error[256]={0};
    uint8_t *snapshot=NULL;size_t length=0;
    CHECK(raft_snapwal_open(w,dir,1,error,sizeof(error))==0);
    CHECK(raft_node_init(n,1)==RAFT_OK);
    CHECK(raft_snapwal_load(w,n,found,&snapshot,&length,error,sizeof(error))==0);
    free(snapshot);
}
static void make_snapshot(raft_node_t *n,raft_kv_store_t *kv,
                          uint8_t **data,size_t *length)
{
    raft_snapshot_t snap;
    raft_snapshot_init(&snap);
    CHECK(raft_snapshot_capture(n,kv,&snap)==RAFT_OK);
    CHECK(raft_snapshot_encode(&snap,data,length)==RAFT_OK);
    CHECK(raft_snapshot_compact_applied(n,&snap)==RAFT_OK);
    raft_snapshot_destroy(&snap);
}
static void create_checkpoint(const char *dir)
{
    raft_snapwal_t w;
    raft_node_t n;
    int found=0;
    char error[256]={0};
    open_and_load(&w,dir,&n,&found);
    CHECK(found==0);
    CHECK(n.current_term==0);
    n.current_term=3;n.voted_for=1;
    const uint8_t command[]={'S','E','T',' ','x',' ','A',0,'B'};
    CHECK(raft_log_append(&n.log,3,command,sizeof(command))==RAFT_OK);
    n.commit_index=1;
    CHECK(raft_snapwal_save(&w,&n,error,sizeof(error))==0);
    CHECK(w.sequence==1);
    raft_kv_store_t kv;raft_kv_init(&kv);
    raft_index_t applied=0;
    CHECK(raft_apply_committed(&n,&kv,&applied)==RAFT_OK && applied==1);
    const uint8_t *val=NULL;size_t value_len=0;
    CHECK(raft_kv_get(&kv,(const uint8_t *)"x",1,&val,&value_len)==1);
    CHECK(value_len==3 && val[1]==0);
    uint8_t *snapshot=NULL;size_t length=0;
    make_snapshot(&n,&kv,&snapshot,&length);
    CHECK(n.log.base_index==1 && n.log.count==0);
    CHECK(raft_snapwal_save(&w,&n,error,sizeof(error))!=0); /* requires checkpoint */
    if(raft_snapwal_checkpoint(&w,&n,snapshot,length,error,sizeof(error))!=0) { fprintf(stderr,"checkpoint error: %s\n",error);exit(1); }
    CHECK(w.sequence==1);
    CHECK(raft_log_append(&n.log,3,"SET y 2",7)==RAFT_OK);
    n.commit_index=2;
    CHECK(raft_snapwal_save(&w,&n,error,sizeof(error))==0);
    CHECK(w.sequence==2);
    CHECK(raft_snapwal_save(&w,&n,error,sizeof(error))==0 && w.sequence==2);
    CHECK(raft_log_append(&n.log,3,"SET pending 8",13)==RAFT_OK);
    CHECK(raft_snapwal_save(&w,&n,error,sizeof(error))==0);
    CHECK(w.sequence==3);
    raft_kv_destroy(&kv);
    raft_node_destroy(&n);
    raft_snapwal_close(&w);
    free(snapshot);
}
static void verify_restart(const char *dir)
{
    char error[256]={0};
    raft_snapwal_t w;
    raft_node_t n;
    CHECK(raft_snapwal_open(&w,dir,1,error,sizeof(error))==0);
    CHECK(raft_node_init(&n,1)==RAFT_OK);
    int found=0;uint8_t *bytes=NULL;size_t len=0;
    CHECK(raft_snapwal_load(&w,&n,&found,&bytes,&len,error,sizeof(error))==0);
    CHECK(found==1 && len>0 && n.log.base_index==1 && n.log.base_term==3);
    CHECK(n.current_term==3 && n.voted_for==1);
    CHECK(n.commit_index==2 && raft_log_last_index(&n.log)==3);
    raft_snapshot_t snap;raft_snapshot_init(&snap);
    CHECK(raft_snapshot_decode(bytes,len,&snap)==RAFT_OK);
    raft_node_t fresh;
    CHECK(raft_node_init(&fresh,1)==RAFT_OK);
    fresh.current_term=n.current_term;
    raft_kv_store_t kv;raft_kv_init(&kv);
    CHECK(raft_snapshot_restore_fresh(&snap,&fresh,&kv)==RAFT_OK);
    n.last_applied=snap.last_index;
    raft_index_t applied=0;
    CHECK(raft_apply_committed(&n,&kv,&applied)==RAFT_OK && applied==1);
    const uint8_t *val=NULL;size_t vlen=0;
    CHECK(raft_kv_get(&kv,(const uint8_t *)"x",1,&val,&vlen)==1);
    CHECK(vlen==3 && val[0]=='A' && val[1]==0 && val[2]=='B');
    CHECK(raft_kv_get(&kv,(const uint8_t *)"y",1,&val,&vlen)==1);
    CHECK(vlen==1 && val[0]=='2');
    CHECK(raft_kv_get(&kv,(const uint8_t *)"pending",7,&val,&vlen)==0);
    raft_kv_destroy(&kv);raft_node_destroy(&fresh);
    raft_snapshot_destroy(&snap);free(bytes);
    raft_node_destroy(&n);raft_snapwal_close(&w);
}
static void test_torn_and_crc(const char *dir)
{
    char file[512];
    (void)snprintf(file,sizeof(file),"%s/node-1.snapwal",dir);
    int fd=open(file,O_WRONLY|O_APPEND);
    CHECK(fd>=0);
    const uint8_t torn[]={0,0,0,72,0,0,0,0,1,2};
    CHECK(write(fd,torn,sizeof(torn))==(ssize_t)sizeof(torn));
    CHECK(close(fd)==0);
    verify_restart(dir); /* torn final record discarded */
    fd=open(file,O_RDWR);CHECK(fd>=0);
    struct stat st;CHECK(fstat(fd,&st)==0);
    CHECK(st.st_size>120);
    off_t offset=24;
    uint8_t byte;
    CHECK(pread(fd,&byte,1,offset)==1);
    byte^=0x20;
    CHECK(pwrite(fd,&byte,1,offset)==1);
    CHECK(close(fd)==0);
    raft_snapwal_t w;
    char error[256]={0};
    CHECK(raft_snapwal_open(&w,dir,1,error,sizeof(error))==0);
    raft_node_t n;CHECK(raft_node_init(&n,1)==RAFT_OK);
    n.current_term=5;
    int found=0;uint8_t *snap=NULL;size_t len=0;
    CHECK(raft_snapwal_load(&w,&n,&found,&snap,&len,error,sizeof(error))!=0);
    CHECK(n.current_term==5 && n.log.count==0); /* unchanged on bad CRC */
    CHECK(snap==NULL);
    raft_node_destroy(&n);raft_snapwal_close(&w);
}
int main(void)
{
    char dir[]="/tmp/raft-phase21-unit-XXXXXX";
    CHECK(mkdtemp(dir)!=NULL);
    create_checkpoint(dir);
    verify_restart(dir);
    test_torn_and_crc(dir);
    char file[512];
    (void)snprintf(file,sizeof(file),"%s/node-1.snapwal",dir);(void)unlink(file);
    (void)snprintf(file,sizeof(file),"%s/node-1.lock",dir);(void)unlink(file);
    (void)rmdir(dir);
    printf("PASS: %u Phase 21 snapshot/WAL recovery assertions\n",checks);
    return 0;
}
