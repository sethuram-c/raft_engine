#define _POSIX_C_SOURCE 200809L
#include "raft_snapwal.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#define HEADER_SIZE 20u
#define RECORD_FIXED 60u
#define RECORD_MAX (RAFT_STORAGE_MAX_BYTES + RAFT_SNAPSHOT_MAX_BYTES + 96u)
#define SNAPWAL_FILE_LIMIT (64u * 1024u * 1024u)
#define FULL_RECORD 1u
#define DELTA_RECORD 2u
static const uint8_t magic[8] = {'R','A','F','T','2','1','W','L'};

static void fail(char *buf, size_t cap, const char *fmt, ...)
{
    if (!buf || !cap) return;
    va_list ap;
    va_start(ap,fmt);
    (void)vsnprintf(buf,cap,fmt,ap);
    va_end(ap);
}
static void p32(uint8_t *p,uint32_t x)
{ for(unsigned i=0;i<4;i++) p[i]=(uint8_t)(x>>(24u-8u*i)); }
static void p64(uint8_t *p,uint64_t x)
{ for(unsigned i=0;i<8;i++) p[i]=(uint8_t)(x>>(56u-8u*i)); }
static uint32_t g32(const uint8_t *p)
{ uint32_t x=0;for(unsigned i=0;i<4;i++) x=(x<<8)|p[i];return x; }
static uint64_t g64(const uint8_t *p)
{ uint64_t x=0;for(unsigned i=0;i<8;i++) x=(x<<8)|p[i];return x; }
static uint32_t crc32(const uint8_t *p,size_t n)
{
    uint32_t x=UINT32_MAX;
    for(size_t i=0;i<n;i++) {
        x^=p[i];
        for(unsigned j=0;j<8;j++) x=(x>>1)^(0xedb88320u&(0u-(x&1u)));
    }
    return ~x;
}
static int write_all(int fd,const uint8_t *p,size_t n)
{
    for(size_t at=0;at<n;) {
        ssize_t r=write(fd,p+at,n-at);
        if (r<0 && errno==EINTR) continue;
        if (r<=0) return -1;
        at+=(size_t)r;
    }
    return 0;
}
static int read_all(int fd,uint8_t *p,size_t n)
{
    for(size_t at=0;at<n;) {
        ssize_t r=read(fd,p+at,n-at);
        if(r<0 && errno==EINTR) continue;
        if(r<=0) return -1;
        at+=(size_t)r;
    }
    return 0;
}
static int valid_state(const raft_node_t *n)
{
    if (!n || !raft_node_is_valid(n) ||
        n->log.count>RAFT_STORAGE_MAX_ENTRIES ||
        n->commit_index<n->log.base_index ||
        n->log.base_term>n->current_term ||
        (n->log.base_index==0)!=(n->log.base_term==0)) return 0;
    for(size_t i=0;i<n->log.count;i++)
        if(n->log.entries[i].command_len>RAFT_STORAGE_MAX_COMMAND) return 0;
    return 1;
}
static int equal_entry(const raft_log_entry_t *a,const raft_log_entry_t *b)
{
    return a->term==b->term && a->command_len==b->command_len &&
           (!a->command_len || !memcmp(a->command,b->command,a->command_len));
}
static int clone_node(raft_node_t *out,const raft_node_t *in)
{
    raft_node_t tmp;
    if(raft_node_init(&tmp,in->id)!=RAFT_OK) return -1;
    tmp.current_term=in->current_term;
    tmp.voted_for=in->voted_for;
    tmp.commit_index=in->commit_index;
    tmp.last_applied=in->log.base_index;
    tmp.log.base_index=in->log.base_index;
    tmp.log.base_term=in->log.base_term;
    for(size_t i=0;i<in->log.count;i++) {
        const raft_log_entry_t *e=&in->log.entries[i];
        if(raft_log_append(&tmp.log,e->term,e->command,e->command_len)!=RAFT_OK) {
            raft_node_destroy(&tmp);return -1;
        }
    }
    if(!valid_state(&tmp)) {raft_node_destroy(&tmp);return -1;}
    raft_node_destroy(out);*out=tmp;return 0;
}
static int validate_snapshot(const uint8_t *bytes,size_t length,
                             raft_index_t index,raft_term_t term)
{
    if (!index && !term && !length) return 1;
    if (!bytes || !length || length>RAFT_SNAPSHOT_MAX_BYTES ||
        !index || !term) return 0;
    raft_snapshot_t snapshot;
    raft_snapshot_init(&snapshot);
    int ok=raft_snapshot_decode(bytes,length,&snapshot)==RAFT_OK &&
           snapshot.last_index==index && snapshot.last_term==term;
    raft_snapshot_destroy(&snapshot);
    return ok;
}
static int encode_record(const raft_node_t *node,size_t keep,
                         uint64_t seq,unsigned mode,
                         const uint8_t *snapshot,size_t snap_len,
                         uint8_t **output,size_t *output_len,
                         char *error,size_t cap)
{
    if (!valid_state(node) || seq==0 || keep>node->log.count ||
        (mode!=FULL_RECORD && mode!=DELTA_RECORD) ||
        (mode==DELTA_RECORD && snap_len!=0)) {
        fail(error,cap,"invalid snapshot WAL record state");return -1;
    }
    if(mode==FULL_RECORD && (keep!=0 ||
       !validate_snapshot(snapshot,snap_len,node->log.base_index,node->log.base_term))) {
        fail(error,cap,"checkpoint snapshot does not match log boundary");return -1;
    }
    size_t len=RECORD_FIXED+snap_len;
    for(size_t i=keep;i<node->log.count;i++) {
        size_t n=node->log.entries[i].command_len;
        if (n>RAFT_STORAGE_MAX_COMMAND || len>RECORD_MAX-12u-n) {
            fail(error,cap,"snapshot WAL record too large");return -1;
        }
        len+=12u+n;
    }
    if(len>RECORD_MAX || node->log.count-keep>UINT32_MAX || snap_len>UINT32_MAX) {
        fail(error,cap,"snapshot WAL record exceeds limit");return -1;
    }
    uint8_t *frame=malloc(8+len);
    if(!frame) {fail(error,cap,"allocation failed");return -1;}
    uint8_t *p=frame+8;
    p64(p,seq);p[8]=(uint8_t)mode;
    memset(p+9,0,3);
    p64(p+12,node->current_term);p32(p+20,node->voted_for);
    p64(p+24,node->commit_index);
    p64(p+32,node->log.base_index);p64(p+40,node->log.base_term);
    p32(p+48,(uint32_t)keep);
    p32(p+52,(uint32_t)(node->log.count-keep));
    p32(p+56,(uint32_t)snap_len);
    size_t at=RECORD_FIXED;
    if(snap_len) {memcpy(p+at,snapshot,snap_len);at+=snap_len;}
    for(size_t i=keep;i<node->log.count;i++) {
        const raft_log_entry_t *e=&node->log.entries[i];
        p64(p+at,e->term);p32(p+at+8,(uint32_t)e->command_len);at+=12;
        if(e->command_len) memcpy(p+at,e->command,e->command_len);
        at+=e->command_len;
    }
    p32(frame,(uint32_t)len);p32(frame+4,crc32(p,len));
    *output=frame;*output_len=8+len;return 0;
}
static int apply_record(raft_node_t *node,const uint8_t *p,size_t len,
                        uint64_t seq,char *error,size_t cap)
{
    if(len<RECORD_FIXED || g64(p)!=seq ||
       p[9] || p[10] || p[11] ||
       (p[8]!=FULL_RECORD && p[8]!=DELTA_RECORD)) {
        fail(error,cap,"bad snapshot WAL sequence/mode");return -1;
    }
    unsigned mode=p[8];
    raft_term_t term=g64(p+12);
    raft_node_id_t vote=g32(p+20);
    raft_index_t commit=g64(p+24);
    raft_index_t base=g64(p+32);
    raft_term_t base_term=g64(p+40);
    uint32_t keep=g32(p+48),added=g32(p+52),snapshot_len=g32(p+56);
    if ((mode==FULL_RECORD && (seq!=1 || keep!=0)) ||
        (mode==DELTA_RECORD && (seq==1 || snapshot_len!=0 ||
         base!=node->log.base_index || base_term!=node->log.base_term)) ||
        term<node->current_term || commit<node->commit_index ||
        (term==node->current_term && node->voted_for!=RAFT_NO_VOTE &&
         node->voted_for!=vote) ||
        keep>node->log.count ||
        (mode==DELTA_RECORD && node->log.base_index+keep<node->commit_index) ||
        added>RAFT_STORAGE_MAX_ENTRIES ||
        (size_t)keep+(size_t)added>RAFT_STORAGE_MAX_ENTRIES ||
        (size_t)snapshot_len>len-RECORD_FIXED ||
        (mode==FULL_RECORD && !validate_snapshot(p+RECORD_FIXED,snapshot_len,base,base_term)) ||
        (mode==DELTA_RECORD && !valid_state(node))) {
        fail(error,cap,"invalid snapshot WAL transition/checkpoint");return -1;
    }
    raft_node_t tmp;
    if(raft_node_init(&tmp,node->id)!=RAFT_OK) return -1;
    tmp.current_term=term;tmp.voted_for=vote;tmp.commit_index=commit;
    tmp.last_applied=base;
    tmp.log.base_index=base;tmp.log.base_term=base_term;
    if(mode==DELTA_RECORD) {
        for(size_t i=0;i<keep;i++) {
            const raft_log_entry_t *e=&node->log.entries[i];
            if(raft_log_append(&tmp.log,e->term,e->command,e->command_len)!=RAFT_OK) {
                raft_node_destroy(&tmp);return -1;
            }
        }
    }
    size_t at=RECORD_FIXED+snapshot_len;
    uint32_t processed=0;
    for(uint32_t i=0;i<added;i++) {
        if(at>len || len-at<12) break;
        raft_term_t entry_term=g64(p+at);
        uint32_t n=g32(p+at+8);
        at+=12;
        if(n>RAFT_STORAGE_MAX_COMMAND || n>len-at ||
           raft_log_append(&tmp.log,entry_term,p+at,n)!=RAFT_OK) break;
        at+=n;
        ++processed;
    }
    if(processed!=added || at!=len || !valid_state(&tmp) ||
       (mode==FULL_RECORD && (base<node->log.base_index ||
        (seq!=1 && commit<node->commit_index)))) {
        fail(error,cap,"invalid snapshot WAL entry/replay state");
        raft_node_destroy(&tmp);return -1;
    }
    raft_node_destroy(node);*node=tmp;return 0;
}
void raft_snapwal_close(raft_snapwal_t *wal)
{
    if(!wal) return;
    if(wal->fd>=0) (void)close(wal->fd);
    wal->fd=-1;
    raft_node_destroy(&wal->previous);
    free(wal->snapshot);wal->snapshot=NULL;wal->snapshot_len=0;
    raft_storage_close(&wal->lock);
    wal->loaded=0;
}
int raft_snapwal_open(raft_snapwal_t *wal,const char *dir,raft_node_id_t id,
                      char *error,size_t cap)
{
    if(!wal || !dir || !*dir || id==RAFT_NO_VOTE) {
        fail(error,cap,"invalid snapshot WAL arguments");return -1;
    }
    memset(wal,0,sizeof(*wal));
    wal->fd=-1;wal->lock.directory_fd=-1;wal->lock.lock_fd=-1;
    if(raft_node_init(&wal->previous,id)!=RAFT_OK) return -1;
    if(raft_storage_open(&wal->lock,dir,id,error,cap)!=0) {
        raft_snapwal_close(wal);return -1;
    }
    /* Do not silently fork a persistent Raft identity across formats. */
    char old_wal[48];
    (void)snprintf(old_wal,sizeof(old_wal),"node-%" PRIu32 ".wal",id);
    struct stat st;
    if(fstatat(wal->lock.directory_fd,old_wal,&st,AT_SYMLINK_NOFOLLOW)==0 ||
       errno!=ENOENT ||
       fstatat(wal->lock.directory_fd,wal->lock.data_name,&st,AT_SYMLINK_NOFOLLOW)==0 ||
       errno!=ENOENT) {
        fail(error,cap,"existing Phase 16/17 disk format: use a separate directory");
        raft_snapwal_close(wal);return -1;
    }
    (void)snprintf(wal->file_name,sizeof(wal->file_name),
                   "node-%" PRIu32 ".snapwal",id);
    (void)snprintf(wal->temp_name,sizeof(wal->temp_name),
                   "node-%" PRIu32 ".snapwal.tmp",id);
    int fresh=0;
    wal->fd=openat(wal->lock.directory_fd,wal->file_name,
                   O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(wal->fd>=0) fresh=1;
    else if(errno==EEXIST)
        wal->fd=openat(wal->lock.directory_fd,wal->file_name,
                       O_RDWR|O_NOFOLLOW|O_CLOEXEC);
    if(wal->fd<0 || fstat(wal->fd,&st)!=0 || !S_ISREG(st.st_mode)) {
        fail(error,cap,"cannot open regular snapshot WAL");
        raft_snapwal_close(wal);return -1;
    }
    if(fresh) {
        uint8_t header[HEADER_SIZE];
        memcpy(header,magic,8);p32(header+8,1);p32(header+12,id);
        p32(header+16,crc32(header,16));
        if(write_all(wal->fd,header,sizeof(header))!=0 ||
           fsync(wal->fd)!=0 || fsync(wal->lock.directory_fd)!=0) {
            fail(error,cap,"cannot persist snapshot WAL header");
            raft_snapwal_close(wal);return -1;
        }
    }
    /* An un-renamed temporary generation was never committed. */
    if(unlinkat(wal->lock.directory_fd,wal->temp_name,0)!=0 && errno!=ENOENT) {
        fail(error,cap,"cannot remove interrupted WAL checkpoint");
        raft_snapwal_close(wal);return -1;
    }
    if(error && cap) error[0]='\0';
    return 0;
}
int raft_snapwal_load(raft_snapwal_t *wal,raft_node_t *node,int *found,
                      uint8_t **snapshot,size_t *length,char *error,size_t cap)
{
    if(!wal || wal->fd<0 || wal->loaded || !node || !found || !snapshot ||
       !length || node->id!=wal->lock.id) {
        fail(error,cap,"invalid snapshot WAL load arguments");return -1;
    }
    *found=0;*snapshot=NULL;*length=0;
    struct stat st;
    if(fstat(wal->fd,&st)!=0 || st.st_size<HEADER_SIZE ||
       st.st_size>SNAPWAL_FILE_LIMIT) {
        fail(error,cap,"invalid snapshot WAL file length");return -1;
    }
    size_t bytes=(size_t)st.st_size;
    uint8_t *all=malloc(bytes);
    if(!all) return -1;
    if(lseek(wal->fd,0,SEEK_SET)<0 || read_all(wal->fd,all,bytes)!=0 ||
       memcmp(all,magic,8)!=0 || g32(all+8)!=1 ||
       g32(all+12)!=wal->lock.id || crc32(all,16)!=g32(all+16)) {
        fail(error,cap,"invalid snapshot WAL header/node ID/CRC");
        free(all);return -1;
    }
    raft_node_t recovered;
    (void)raft_node_init(&recovered,node->id);
    uint8_t *loaded_snapshot=NULL;
    size_t loaded_length=0;
    size_t at=HEADER_SIZE;
    uint64_t seq=0;
    int bad=0,incomplete=0;
    while(at<bytes) {
        size_t remaining=bytes-at;
        if(remaining<8) {incomplete=1;break;}
        uint32_t n=g32(all+at);
        if(n<RECORD_FIXED || n>RECORD_MAX) {
            fail(error,cap,"invalid snapshot WAL record length");bad=1;break;
        }
        if(n>remaining-8) {incomplete=1;break;}
        const uint8_t *p=all+at+8;
        if(crc32(p,n)!=g32(all+at+4) || seq==UINT64_MAX) {
            fail(error,cap,"invalid complete snapshot WAL CRC/sequence");bad=1;break;
        }
        if(seq==0 && (p[8]!=FULL_RECORD || g64(p)!=1)) {
            fail(error,cap,"snapshot WAL must start with full record");bad=1;break;
        }
        if(apply_record(&recovered,p,n,seq+1,error,cap)!=0) {bad=1;break;}
        if(seq==0) {
            uint32_t s=g32(p+56);
            if(s) {
                loaded_snapshot=malloc(s);
                if(!loaded_snapshot) {bad=1;break;}
                memcpy(loaded_snapshot,p+RECORD_FIXED,s);
                loaded_length=s;
            }
        }
        ++seq;at+=8+(size_t)n;
    }
    free(all);
    if(!bad && incomplete && (ftruncate(wal->fd,(off_t)at)!=0 ||
                              fsync(wal->fd)!=0)) {
        fail(error,cap,"could not truncate torn snapshot WAL tail");bad=1;
    }
    if(!bad && clone_node(&wal->previous,&recovered)!=0) {
        fail(error,cap,"snapshot WAL mirror allocation failed");bad=1;
    }
    if(bad || lseek(wal->fd,0,SEEK_END)<0) {
        raft_node_destroy(&recovered);free(loaded_snapshot);return -1;
    }
    uint8_t *copy=NULL;
    if (loaded_length) {
        copy=malloc(loaded_length);
        if (!copy) {
            fail(error,cap,"snapshot copy allocation failed");
            raft_node_destroy(&recovered);
            free(loaded_snapshot);
            return -1;
        }
        memcpy(copy,loaded_snapshot,loaded_length);
    }
    raft_node_destroy(node);*node=recovered;
    wal->snapshot=loaded_snapshot;wal->snapshot_len=loaded_length;
    wal->sequence=seq;wal->loaded=1;
    *found=seq!=0;
    *snapshot=copy;*length=loaded_length;
    if(error && cap) error[0]='\0';
    return 0;
}
int raft_snapwal_checkpoint(raft_snapwal_t *wal,const raft_node_t *node,
                            const uint8_t *snapshot,size_t length,
                            char *error,size_t cap)
{
    if(!wal || wal->fd<0 || !wal->loaded || !valid_state(node) ||
       node->id!=wal->lock.id ||
       !validate_snapshot(snapshot,length,node->log.base_index,node->log.base_term) ||
       node->current_term<wal->previous.current_term ||
       node->commit_index<wal->previous.commit_index ||
       (node->current_term==wal->previous.current_term &&
        wal->previous.voted_for!=RAFT_NO_VOTE &&
        wal->previous.voted_for!=node->voted_for)) {
        fail(error,cap,"unsafe snapshot WAL checkpoint transition");return -1;
    }
    raft_node_t mirror;
    (void)raft_node_init(&mirror,node->id);
    if(clone_node(&mirror,node)!=0) {fail(error,cap,"snapshot mirror OOM");return -1;}
    uint8_t *frame=NULL;
    size_t frame_len=0;
    if(encode_record(node,0,1,FULL_RECORD,snapshot,length,&frame,&frame_len,
                     error,cap)!=0) {
        raft_node_destroy(&mirror);return -1;
    }
    uint8_t *snapshot_copy=NULL;
    if(length) {
        snapshot_copy=malloc(length);
        if(!snapshot_copy) {
            fail(error,cap,"snapshot copy OOM");free(frame);
            raft_node_destroy(&mirror);return -1;
        }
        memcpy(snapshot_copy,snapshot,length);
    }
    uint8_t header[HEADER_SIZE];
    memcpy(header,magic,8);p32(header+8,1);p32(header+12,wal->lock.id);
    p32(header+16,crc32(header,16));
    int temp=-1;
    if(unlinkat(wal->lock.directory_fd,wal->temp_name,0)!=0 && errno!=ENOENT) {
        fail(error,cap,"could not remove old checkpoint temporary file");goto fail;
    }
    temp=openat(wal->lock.directory_fd,wal->temp_name,
                O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(temp<0 || write_all(temp,header,sizeof(header))!=0 ||
       write_all(temp,frame,frame_len)!=0 || fsync(temp)!=0) {
        fail(error,cap,"cannot write/fsync snapshot WAL checkpoint: %s",strerror(errno));
        goto fail;
    }
    if(renameat(wal->lock.directory_fd,wal->temp_name,
                wal->lock.directory_fd,wal->file_name)!=0 ||
       fsync(wal->lock.directory_fd)!=0) {
        fail(error,cap,"cannot rename/fsync snapshot WAL checkpoint: %s",strerror(errno));
        goto fail; /* possible ambiguity: server must fail-stop */
    }
    (void)close(wal->fd);
    wal->fd=temp;
    free(wal->snapshot);wal->snapshot=snapshot_copy;
    wal->snapshot_len=length;
    raft_node_destroy(&wal->previous);wal->previous=mirror;
    wal->sequence=1;
    free(frame);
    if(lseek(wal->fd,0,SEEK_END)<0) {
        fail(error,cap,"cannot seek checkpoint end");return -1;
    }
    if(error && cap) error[0]='\0';
    return 0;
fail:
    if(temp>=0) (void)close(temp);
    (void)unlinkat(wal->lock.directory_fd,wal->temp_name,0);
    free(frame);free(snapshot_copy);raft_node_destroy(&mirror);
    return -1;
}
int raft_snapwal_save(raft_snapwal_t *wal,const raft_node_t *node,
                      char *error,size_t cap)
{
    if(!wal || wal->fd<0 || !wal->loaded || !valid_state(node) ||
       node->id!=wal->lock.id ||
       node->log.base_index!=wal->previous.log.base_index ||
       node->log.base_term!=wal->previous.log.base_term ||
       node->current_term<wal->previous.current_term ||
       node->commit_index<wal->previous.commit_index ||
       (node->current_term==wal->previous.current_term &&
        wal->previous.voted_for!=RAFT_NO_VOTE &&
        node->voted_for!=wal->previous.voted_for)) {
        fail(error,cap,"unsafe snapshot WAL append/base changed without checkpoint");return -1;
    }
    if(wal->sequence==UINT64_MAX) return -1;
    size_t keep=0;
    while(keep<wal->previous.log.count && keep<node->log.count &&
          equal_entry(&wal->previous.log.entries[keep],&node->log.entries[keep])) ++keep;
    if(wal->previous.log.base_index+keep<wal->previous.commit_index) {
        fail(error,cap,"cannot erase committed WAL entries");return -1;
    }
    if(wal->sequence && keep==node->log.count &&
       keep==wal->previous.log.count &&
       node->current_term==wal->previous.current_term &&
       node->voted_for==wal->previous.voted_for &&
       node->commit_index==wal->previous.commit_index) return 0;
    raft_node_t mirror;
    (void)raft_node_init(&mirror,node->id);
    if(clone_node(&mirror,node)!=0) return -1;
    uint8_t *frame=NULL;
    size_t frame_len=0;
    unsigned mode=wal->sequence==0?FULL_RECORD:DELTA_RECORD;
    if(encode_record(node,wal->sequence?keep:0,wal->sequence+1,mode,
       NULL,0,&frame,&frame_len,error,cap)!=0) {
        raft_node_destroy(&mirror);return -1;
    }
    struct stat st;
    if(fstat(wal->fd,&st)!=0 || st.st_size<0 ||
       (uint64_t)st.st_size+frame_len>SNAPWAL_FILE_LIMIT ||
       lseek(wal->fd,0,SEEK_END)<0 ||
       write_all(wal->fd,frame,frame_len)!=0 || fsync(wal->fd)!=0) {
        fail(error,cap,"cannot append/fsync snapshot WAL record: %s",strerror(errno));
        free(frame);raft_node_destroy(&mirror);return -1;
    }
    free(frame);
    raft_node_destroy(&wal->previous);wal->previous=mirror;
    ++wal->sequence;
    /* Periodic records-of-a-generation compaction is safe because the
     * atomic checkpoint still contains the same validated snapshot. */
    if(wal->sequence>=64)
        return raft_snapwal_checkpoint(wal,node,wal->snapshot,
                                      wal->snapshot_len,error,cap);
    if(error && cap) error[0]='\0';
    return 0;
}
