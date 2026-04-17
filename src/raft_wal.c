#define _POSIX_C_SOURCE 200809L
#include "raft_wal.h"
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

/* All integers are big endian. Header: magic[8], version:u32, id:u32,
 * CRC32:u32. Record: payload_len:u32, payload_crc:u32, payload:
 * sequence:u64, term:u64, vote:u32, commit:u64, keep:u32, suffix:u32,
 * (entry_term:u64, command_len:u32, command_bytes)*suffix. */
#define WAL_HEADER 20u
#define RECORD_FIXED 36u
#define RECORD_MAX (RAFT_STORAGE_MAX_BYTES + 64u)
static const uint8_t wal_magic[8] = {'R','A','F','T','1','7','W','L'};

static void fail(char *err, size_t cap, const char *fmt, ...)
{
    if (!err || !cap) return;
    va_list ap; va_start(ap, fmt);
    (void)vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}
static void p32(uint8_t *p, uint32_t x)
{
    for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(x>>(24-8*i));
}
static void p64(uint8_t *p, uint64_t x)
{
    for (unsigned i=0;i<8;i++) p[i]=(uint8_t)(x>>(56-8*i));
}
static uint32_t g32(const uint8_t *p)
{
    uint32_t x=0; for (unsigned i=0;i<4;i++) x=(x<<8)|p[i]; return x;
}
static uint64_t g64(const uint8_t *p)
{
    uint64_t x=0; for (unsigned i=0;i<8;i++) x=(x<<8)|p[i]; return x;
}
static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t crc=UINT32_MAX;
    for(size_t i=0;i<n;i++) {
        crc^=p[i];
        for(unsigned b=0;b<8;b++) crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));
    }
    return ~crc;
}
static int full_write(int fd, const uint8_t *p, size_t n)
{
    size_t at=0;
    while(at<n) {
        ssize_t got=write(fd,p+at,n-at);
        if (got<0 && errno==EINTR) continue;
        if (got<=0) return -1;
        at+=(size_t)got;
    }
    return 0;
}
static int full_read(int fd, uint8_t *p, size_t n)
{
    size_t at=0;
    while(at<n) {
        ssize_t got=read(fd,p+at,n-at);
        if (got<0 && errno==EINTR) continue;
        if (got<=0) return -1;
        at+=(size_t)got;
    }
    return 0;
}
static int copy_state(raft_node_t *to, const raft_node_t *from)
{
    raft_node_t fresh;
    if (raft_node_init(&fresh,from->id)!=RAFT_OK) return -1;
    fresh.current_term=from->current_term;
    fresh.voted_for=from->voted_for;
    fresh.commit_index=from->commit_index;
    for (size_t i=0;i<from->log.count;i++) {
        const raft_log_entry_t *e=&from->log.entries[i];
        if (raft_log_append(&fresh.log,e->term,e->command,e->command_len)!=RAFT_OK) {
            raft_node_destroy(&fresh);
            return -1;
        }
    }
    if (!raft_node_is_valid(&fresh)) { raft_node_destroy(&fresh); return -1; }
    raft_node_destroy(to);
    *to=fresh;
    return 0;
}
static int entry_equal(const raft_log_entry_t *a, const raft_log_entry_t *b)
{
    return a->term==b->term && a->command_len==b->command_len &&
           (!a->command_len || memcmp(a->command,b->command,a->command_len)==0);
}
static int state_valid(const raft_node_t *n)
{
    if (!n || !raft_node_is_valid(n) || n->log.count>RAFT_STORAGE_MAX_ENTRIES) return 0;
    for(size_t i=0;i<n->log.count;i++)
        if (n->log.entries[i].command_len>RAFT_STORAGE_MAX_COMMAND) return 0;
    return 1;
}

void raft_wal_close(raft_wal_t *wal)
{
    if (!wal) return;
    if (wal->fd>=0) (void)close(wal->fd);
    wal->fd=-1;
    raft_node_destroy(&wal->previous);
    raft_storage_close(&wal->lock);
    wal->loaded=0;
}

int raft_wal_open(raft_wal_t *wal, const char *directory, raft_node_id_t id,
                  char *error, size_t cap)
{
    if (!wal || !directory || id==RAFT_NO_VOTE) {
        fail(error,cap,"invalid WAL parameters"); return -1;
    }
    memset(wal,0,sizeof(*wal));
    wal->fd=-1;
    wal->lock.directory_fd=-1;
    wal->lock.lock_fd=-1;
    if (raft_node_init(&wal->previous,id)!=RAFT_OK) {
        fail(error,cap,"cannot initialize WAL mirror"); return -1;
    }
    if (raft_storage_open(&wal->lock,directory,id,error,cap)!=0) {
        raft_wal_close(wal); return -1;
    }
    /* Never silently fork one voting identity into incompatible disk states. */
    struct stat st;
    if (fstatat(wal->lock.directory_fd,wal->lock.data_name,&st,AT_SYMLINK_NOFOLLOW)==0 ||
        errno!=ENOENT) {
        fail(error,cap,"Phase 16 snapshot exists (or cannot be checked); use a separate WAL directory");
        raft_wal_close(wal); return -1;
    }
    (void)snprintf(wal->file_name,sizeof(wal->file_name),"node-%" PRIu32 ".wal",id);
    int fresh=0;
    wal->fd=openat(wal->lock.directory_fd,wal->file_name,
                   O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if (wal->fd>=0) fresh=1;
    else if (errno==EEXIST)
        wal->fd=openat(wal->lock.directory_fd,wal->file_name,
                       O_RDWR|O_NOFOLLOW|O_CLOEXEC);
    if (wal->fd<0 || fstat(wal->fd,&st)!=0 || !S_ISREG(st.st_mode)) {
        fail(error,cap,"cannot open regular WAL file: %s",strerror(errno));
        raft_wal_close(wal); return -1;
    }
    if (fresh) {
        uint8_t header[WAL_HEADER];
        memcpy(header,wal_magic,8);
        p32(header+8,1);
        p32(header+12,id);
        p32(header+16,crc32(header,16));
        if (full_write(wal->fd,header,sizeof(header))!=0 ||
            fsync(wal->fd)!=0 || fsync(wal->lock.directory_fd)!=0) {
            fail(error,cap,"cannot durably create WAL header: %s",strerror(errno));
            raft_wal_close(wal); return -1;
        }
    }
    if (error && cap) error[0]='\0';
    return 0;
}

static int apply_record(raft_node_t *state, const uint8_t *p, size_t len,
                        uint64_t sequence, char *error, size_t cap)
{
    if (len<RECORD_FIXED || g64(p)!=sequence || g64(p+8)<state->current_term) {
        fail(error,cap,"corrupt WAL sequence/term"); return -1;
    }
    raft_term_t term=g64(p+8);
    raft_node_id_t vote=g32(p+16);
    raft_index_t commit=g64(p+20);
    uint32_t keep=g32(p+28), appended=g32(p+32);
    if (keep>state->log.count || keep<state->commit_index ||
        commit<state->commit_index || appended>RAFT_STORAGE_MAX_ENTRIES ||
        (size_t)keep+(size_t)appended>RAFT_STORAGE_MAX_ENTRIES ||
        (term==state->current_term && state->voted_for!=RAFT_NO_VOTE &&
         vote!=state->voted_for)) {
        fail(error,cap,"WAL attempted to roll back committed/voted state"); return -1;
    }
    raft_node_t next;
    if (raft_node_init(&next,state->id)!=RAFT_OK) return -1;
    next.current_term=term;
    next.voted_for=vote;
    next.commit_index=commit;
    int ok=1;
    for(size_t i=0;i<keep;i++) {
        const raft_log_entry_t *e=&state->log.entries[i];
        if (raft_log_append(&next.log,e->term,e->command,e->command_len)!=RAFT_OK)
            {ok=0;break;}
    }
    size_t off=RECORD_FIXED;
    for(uint32_t i=0;ok && i<appended;i++) {
        if (off>len || len-off<12) {ok=0;break;}
        raft_term_t e_term=g64(p+off);
        uint32_t n=g32(p+off+8);
        off+=12;
        if (n>RAFT_STORAGE_MAX_COMMAND || len-off<n ||
            raft_log_append(&next.log,e_term,p+off,n)!=RAFT_OK) {ok=0;break;}
        off+=n;
    }
    if (!ok || off!=len || !state_valid(&next)) {
        fail(error,cap,"invalid WAL delta/entry or final counters");
        raft_node_destroy(&next); return -1;
    }
    raft_node_destroy(state);
    *state=next;
    return 0;
}

int raft_wal_load(raft_wal_t *wal, raft_node_t *node, int *found,
                  char *error, size_t cap)
{
    if (!wal || !node || !found || wal->fd<0 || wal->loaded ||
        node->id!=wal->lock.id) {fail(error,cap,"invalid WAL load state");return -1;}
    *found=0;
    struct stat st;
    if (fstat(wal->fd,&st)!=0 || st.st_size<WAL_HEADER ||
        st.st_size>(off_t)RAFT_WAL_MAX_FILE_BYTES) {
        fail(error,cap,"invalid WAL file length"); return -1;
    }
    size_t bytes=(size_t)st.st_size;
    uint8_t *all=malloc(bytes);
    if (!all) {fail(error,cap,"out of memory reading WAL");return -1;}
    if (lseek(wal->fd,0,SEEK_SET)<0 || full_read(wal->fd,all,bytes)!=0 ||
        memcmp(all,wal_magic,8)!=0 || g32(all+8)!=1 ||
        g32(all+12)!=wal->lock.id || crc32(all,16)!=g32(all+16)) {
        fail(error,cap,"corrupt WAL header/version/CRC or wrong node ID");
        free(all); return -1;
    }
    raft_node_t recovered;
    (void)raft_node_init(&recovered,node->id);
    size_t at=WAL_HEADER;
    uint64_t sequence=0;
    int bad=0, incomplete=0;
    while(at<bytes) {
        size_t remaining=bytes-at;
        if (remaining<8) {incomplete=1;break;}
        uint32_t len=g32(all+at);
        uint32_t crc=g32(all+at+4);
        if (len<RECORD_FIXED || len>RECORD_MAX) {
            bad=1;fail(error,cap,"corrupt WAL record length");break;
        }
        if ((size_t)len>remaining-8) {incomplete=1;break;}
        if (crc32(all+at+8,len)!=crc) {
            bad=1;fail(error,cap,"corrupt complete WAL record CRC");break;
        }
        if (sequence==UINT64_MAX ||
            apply_record(&recovered,all+at+8,len,sequence+1,error,cap)!=0) {
            bad=1;break;
        }
        ++sequence;
        at+=8u+len;
    }
    free(all);
    if (!bad && incomplete) {
        /* Only an incomplete FINAL frame is recoverable; complete CRC errors
         * or semantic violations always fail closed. */
        if (ftruncate(wal->fd,(off_t)at)!=0 || fsync(wal->fd)!=0) {
            fail(error,cap,"could not truncate/fsync interrupted WAL tail");bad=1;
        }
    }
    if (bad || copy_state(&wal->previous,&recovered)!=0) {
        raft_node_destroy(&recovered); return -1;
    }
    raft_node_destroy(node);
    *node=recovered;
    wal->sequence=sequence;
    wal->loaded=1;
    *found=sequence!=0;
    if (lseek(wal->fd,0,SEEK_END)<0) {
        fail(error,cap,"seek to WAL append offset failed");return -1;
    }
    if (error && cap) error[0]='\0';
    return 0;
}

int raft_wal_save(raft_wal_t *wal, const raft_node_t *node,
                  char *error, size_t cap)
{
    if (!wal || wal->fd<0 || !wal->loaded || !state_valid(node) ||
        node->id!=wal->lock.id || node->current_term<wal->previous.current_term ||
        node->commit_index<wal->previous.commit_index ||
        (node->current_term==wal->previous.current_term &&
         wal->previous.voted_for!=RAFT_NO_VOTE &&
         node->voted_for!=wal->previous.voted_for)) {
        fail(error,cap,"invalid or unsafe WAL transition"); return -1;
    }
    const raft_node_t *prev=&wal->previous;
    size_t keep=0;
    while (keep<prev->log.count && keep<node->log.count &&
           entry_equal(&prev->log.entries[keep],&node->log.entries[keep])) ++keep;
    if (keep<prev->commit_index || wal->sequence==UINT64_MAX) {
        fail(error,cap,"cannot truncate committed WAL prefix/overflow");return -1;
    }
    if (keep==prev->log.count && keep==node->log.count &&
        node->current_term==prev->current_term && node->voted_for==prev->voted_for &&
        node->commit_index==prev->commit_index) return 0;

    size_t size=RECORD_FIXED;
    for(size_t i=keep;i<node->log.count;i++) {
        size_t n=node->log.entries[i].command_len;
        if(size>RECORD_MAX-12-n) {fail(error,cap,"WAL record too large");return -1;}
        size+=12+n;
    }
    raft_node_t new_mirror;
    (void)raft_node_init(&new_mirror,node->id);
    if (copy_state(&new_mirror,node)!=0) {
        fail(error,cap,"cannot allocate WAL mirror before append");return -1;
    }
    uint8_t *record=malloc(8+size);
    if (!record) {raft_node_destroy(&new_mirror);fail(error,cap,"out of memory");return -1;}
    uint8_t *p=record+8;
    p64(p,wal->sequence+1);p64(p+8,node->current_term);
    p32(p+16,node->voted_for);p64(p+20,node->commit_index);
    p32(p+28,(uint32_t)keep);p32(p+32,(uint32_t)(node->log.count-keep));
    size_t at=RECORD_FIXED;
    for(size_t i=keep;i<node->log.count;i++) {
        const raft_log_entry_t *e=&node->log.entries[i];
        p64(p+at,e->term);p32(p+at+8,(uint32_t)e->command_len);at+=12;
        if(e->command_len) memcpy(p+at,e->command,e->command_len);
        at+=e->command_len;
    }
    p32(record,(uint32_t)size);p32(record+4,crc32(p,size));
    struct stat st;
    if (fstat(wal->fd,&st)!=0 || st.st_size<0 ||
        (uint64_t)st.st_size+8u+size>RAFT_WAL_MAX_FILE_BYTES ||
        lseek(wal->fd,0,SEEK_END)<0 ||
        full_write(wal->fd,record,size+8)!=0 || fsync(wal->fd)!=0) {
        fail(error,cap,"WAL append/fsync failed (or file limit reached): %s",strerror(errno));
        raft_node_destroy(&new_mirror);free(record);return -1;
    }
    free(record);
    raft_node_destroy(&wal->previous);
    wal->previous=new_mirror;
    ++wal->sequence;
    if(error && cap) error[0]='\0';
    return 0;
}
