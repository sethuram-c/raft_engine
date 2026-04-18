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

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL checkpoint %s:%d: %s\n", __FILE__,__LINE__,#x);exit(1); } } while (0)

static off_t bytes_of(const char *p)
{
    struct stat st;
    CHECK(stat(p,&st)==0);
    return st.st_size;
}
static void newnode(raft_node_t *n)
{
    CHECK(raft_node_init(n,2)==RAFT_OK);
    CHECK(raft_node_start_follower(n,10000,12345)==RAFT_OK);
}
static uint64_t seq_from_disk(const char *p)
{
    unsigned char b[36];
    int fd=open(p,O_RDONLY);
    CHECK(fd>=0 && read(fd,b,sizeof(b))==(ssize_t)sizeof(b));
    CHECK(close(fd)==0);
    CHECK(memcmp(b,"RAFT17WL",8)==0);
    uint64_t value=0;
    for (int i=0;i<8;i++) value=(value<<8)|b[28+i];
    return value;
}
int main(void)
{
    char dir[]="/tmp/raft18-checkpoint-XXXXXX";
    CHECK(mkdtemp(dir)!=NULL);
    char path[256], tmp[256], lock[256], error[256]={0};
    CHECK(snprintf(path,sizeof(path),"%s/node-2.wal",dir)>0);
    CHECK(snprintf(tmp,sizeof(tmp),"%s/node-2.wal.compact.tmp",dir)>0);
    CHECK(snprintf(lock,sizeof(lock),"%s/node-2.lock",dir)>0);
    raft_wal_t wal;
    raft_node_t n;
    int found=0;
    CHECK(raft_wal_open(&wal,dir,2,error,sizeof(error))==0);
    newnode(&n);
    CHECK(raft_wal_load(&wal,&n,&found,error,sizeof(error))==0 && !found);
    CHECK(raft_wal_compact(&wal,error,sizeof(error))==0);
    CHECK(bytes_of(path)==20 && wal.sequence==0);
    puts("PASS checkpoint: empty WAL remains empty");

    n.current_term=3;
    n.voted_for=2;
    CHECK(raft_wal_save(&wal,&n,error,sizeof(error))==0);
    const unsigned char binary[]={'S','E','T',' ','k',' ','A',0,'B'};
    CHECK(raft_log_append(&n.log,3,binary,sizeof(binary))==RAFT_OK);
    CHECK(raft_wal_save(&wal,&n,error,sizeof(error))==0);
    CHECK(raft_log_append(&n.log,3,"SET pending x",13)==RAFT_OK);
    n.commit_index=1;
    CHECK(raft_wal_save(&wal,&n,error,sizeof(error))==0);
    off_t before=bytes_of(path);
    CHECK(wal.sequence==3 && seq_from_disk(path)==1);
    raft_wal_t duplicate;
    CHECK(raft_wal_open(&duplicate,dir,2,error,sizeof(error))!=0);
    CHECK(raft_wal_compact(&wal,error,sizeof(error))==0);
    CHECK(wal.sequence==1 && seq_from_disk(path)==1);
    off_t after=bytes_of(path);
    CHECK(after<before);
    CHECK(after==20+8+36+12+(off_t)sizeof(binary)+12+13);
    puts("PASS checkpoint: atomic full-image checkpoint preserves committed and uncommitted entries");

    /* Checkpoints can be followed by normal Phase 17 delta frames. */
    CHECK(raft_log_truncate_from(&n.log,2)==RAFT_OK);
    n.current_term=4;
    n.voted_for=RAFT_NO_VOTE;
    CHECK(raft_log_append(&n.log,4,"DEL k",5)==RAFT_OK);
    CHECK(raft_wal_save(&wal,&n,error,sizeof(error))==0);
    CHECK(wal.sequence==2);
    raft_wal_close(&wal);
    CHECK(raft_wal_open(&wal,dir,2,error,sizeof(error))==0);
    raft_node_t restored;
    newnode(&restored);
    CHECK(raft_wal_load(&wal,&restored,&found,error,sizeof(error))==0 && found);
    CHECK(restored.current_term==4 && restored.voted_for==RAFT_NO_VOTE);
    CHECK(restored.commit_index==1 && restored.log.count==2);
    CHECK(memcmp(raft_log_at(&restored.log,1)->command,binary,sizeof(binary))==0);
    CHECK(memcmp(raft_log_at(&restored.log,2)->command,"DEL k",5)==0);
    puts("PASS checkpoint: restart + delta replay and uncommitted suffix repair");

    /* Prove automatic checkpointing bounds the count of replayed frames. */
    int saw_reset=0;
    for (unsigned i=0;i<2u*RAFT_WAL_COMPACT_AFTER_RECORDS+11u;i++) {
        restored.current_term++;
        restored.voted_for=2;
        uint64_t prior=wal.sequence;
        CHECK(raft_wal_save(&wal,&restored,error,sizeof(error))==0);
        if (wal.sequence<prior) saw_reset++;
        CHECK(wal.sequence<RAFT_WAL_COMPACT_AFTER_RECORDS);
    }
    CHECK(saw_reset>=2);
    CHECK(bytes_of(path)<4096);
    CHECK(raft_wal_compact(&wal,error,sizeof(error))==0);
    CHECK(wal.sequence==1);
    raft_wal_close(&wal);
    CHECK(raft_wal_open(&wal,dir,2,error,sizeof(error))==0);
    raft_node_t roundtrip;
    newnode(&roundtrip);
    CHECK(raft_wal_load(&wal,&roundtrip,&found,error,sizeof(error))==0);
    CHECK(roundtrip.current_term==restored.current_term);
    CHECK(roundtrip.voted_for==2 && roundtrip.commit_index==1);
    CHECK(roundtrip.log.count==2);
    puts("PASS checkpoint: automatic frame threshold and repeated restart after checkpoint");

    /* Crash before rename: temporary image is NOT authoritative. */
    raft_wal_close(&wal);
    int fd=open(tmp,O_CREAT|O_WRONLY|O_EXCL,0600);
    CHECK(fd>=0);
    CHECK(write(fd,"broken interrupted checkpoint",29)==29);
    CHECK(close(fd)==0);
    CHECK(raft_wal_open(&wal,dir,2,error,sizeof(error))==0);
    CHECK(access(tmp,F_OK)==-1 && errno==ENOENT);
    raft_node_t after_orphan;
    newnode(&after_orphan);
    CHECK(raft_wal_load(&wal,&after_orphan,&found,error,sizeof(error))==0);
    CHECK(after_orphan.current_term==restored.current_term);
    puts("PASS checkpoint: orphan temp removed; complete old WAL remains authoritative");

    /* Checkpoint integrity is enforced by the existing CRC validator. */
    raft_wal_close(&wal);
    fd=open(path,O_RDWR);
    CHECK(fd>=0);
    unsigned char change=0;
    CHECK(pread(fd,&change,1,20+8+8)==1);
    change^=0x11;
    CHECK(pwrite(fd,&change,1,20+8+8)==1);
    CHECK(close(fd)==0);
    CHECK(raft_wal_open(&wal,dir,2,error,sizeof(error))==0);
    raft_node_t corrupted;
    newnode(&corrupted);
    CHECK(raft_wal_load(&wal,&corrupted,&found,error,sizeof(error))!=0);
    CHECK(strstr(error,"corrupt")!=NULL);
    CHECK(corrupted.current_term==0 && corrupted.log.count==0);
    puts("PASS checkpoint: corrupted complete image fails closed without altering node");

    raft_wal_close(&wal);
    raft_node_destroy(&n);
    raft_node_destroy(&restored);
    raft_node_destroy(&roundtrip);
    raft_node_destroy(&after_orphan);
    raft_node_destroy(&corrupted);
    CHECK(unlink(path)==0 && unlink(lock)==0 && rmdir(dir)==0);
    /* Size-based automatic trigger with a small number of large records.
     * Four frames exceed 256 KiB without reaching the 64-record threshold. */
    char large_dir[]="/tmp/raft18-large-XXXXXX";
    CHECK(mkdtemp(large_dir)!=NULL);
    raft_wal_t big;
    CHECK(raft_wal_open(&big,large_dir,2,error,sizeof(error))==0);
    raft_node_t many;
    newnode(&many);
    CHECK(raft_wal_load(&big,&many,&found,error,sizeof(error))==0 && !found);
    many.current_term=1;
    many.voted_for=2;
    unsigned char command[4000];
    memset(command,'X',sizeof(command));
    for (int batch=0;batch<4;batch++) {
        for (int entry=0;entry<24;entry++) {
            command[0]=(unsigned char)('A'+batch);
            CHECK(raft_log_append(&many.log,1,command,sizeof(command))==RAFT_OK);
        }
        CHECK(raft_wal_save(&big,&many,error,sizeof(error))==0);
        if (batch<3) CHECK(big.sequence==(uint64_t)(batch+1));
    }
    CHECK(big.sequence==1); /* the size threshold triggered at frame 4 */
    CHECK(many.log.count==96);
    raft_wal_close(&big);
    CHECK(raft_wal_open(&big,large_dir,2,error,sizeof(error))==0);
    raft_node_t loaded_large;
    newnode(&loaded_large);
    CHECK(raft_wal_load(&big,&loaded_large,&found,error,sizeof(error))==0);
    CHECK(loaded_large.log.count==96 && loaded_large.current_term==1);
    CHECK(raft_log_at(&loaded_large.log,96)->command[0]=='D');
    raft_wal_close(&big);
    raft_node_destroy(&many);
    raft_node_destroy(&loaded_large);
    CHECK(snprintf(path,sizeof(path),"%s/node-2.wal",large_dir)>0);
    CHECK(snprintf(lock,sizeof(lock),"%s/node-2.lock",large_dir)>0);
    CHECK(unlink(path)==0 && unlink(lock)==0 && rmdir(large_dir)==0);
    puts("PASS checkpoint: size-based trigger at 256 KiB with four large records");

    puts("All Phase 18 checkpoint unit tests passed.");
    return 0;
}
