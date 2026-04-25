#define _POSIX_C_SOURCE 200809L
#include "raft_auth.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

static void put(const char *path, const char *content, mode_t mode)
{
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,mode);
    assert(fd>=0);
    assert(fchmod(fd,mode)==0);
    size_t n=strlen(content);
    assert(write(fd,content,n)==(ssize_t)n);
    assert(close(fd)==0);
}
int main(void)
{
    char directory[]="/tmp/raft-auth-unit-XXXXXX";
    assert(mkdtemp(directory));
    char ring[256],secret_path[256];
    assert(snprintf(ring,sizeof(ring),"%s/ring",directory)>0);
    assert(snprintf(secret_path,sizeof(secret_path),"%s/secret",directory)>0);
    const char *a="000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    const char *b="ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
    char text[512];
    snprintf(text,sizeof(text),"90901 %s\n90902 %s\n",a,b);
    put(ring,text,0600);
    put(secret_path,a,0600);
    raft_auth_keys_t keys={0};
    uint8_t key[RAFT_AUTH_KEY_BYTES]={0};
    assert(raft_auth_read_keyring(ring,&keys)==0 && keys.count==2);
    assert(raft_auth_read_secret(secret_path,key)==0);
    assert(memcmp(key,keys.clients[0].key,sizeof(key))==0);
    const char *cmd="SET auth verified";
    uint8_t req[1200];size_t len=0;
    assert(raft_auth_sign(450,90901,1,(const uint8_t *)cmd,strlen(cmd),
                          key,req,sizeof(req),&len)==0);
    assert(len==20+strlen(cmd)+RAFT_AUTH_TAG_BYTES);
    uint32_t ms=0;raft_dedup_entry_t dec={0};
    assert(raft_auth_verify(&keys,req,len,&ms,&dec)==0);
    assert(ms==450 && dec.client_id==90901 && dec.request_id==1 &&
           dec.command_len==strlen(cmd) &&
           memcmp(dec.command,cmd,strlen(cmd))==0);
    /* Changing the claimed client ID, request ID, timeout, command or tag
     * invalidates the MAC. Failed verification must not overwrite outputs. */
    size_t positions[]={0,4,12,20,len-1};
    for(size_t i=0;i<sizeof(positions)/sizeof(positions[0]);i++) {
        uint8_t saved=req[positions[i]];
        req[positions[i]]^=1;
        ms=1234;dec.client_id=42;
        assert(raft_auth_verify(&keys,req,len,&ms,&dec)!=0);
        assert(ms==1234 && dec.client_id==42);
        req[positions[i]]=saved;
    }
    assert(raft_auth_verify(&keys,req,len-1,&ms,&dec)!=0);
    assert(raft_auth_verify(&keys,req,4,&ms,&dec)!=0);
    assert(raft_auth_sign(450,90903,1,(const uint8_t *)cmd,strlen(cmd),
                          key,req,sizeof(req),&len)==0);
    assert(raft_auth_verify(&keys,req,len,&ms,&dec)!=0);
    assert(raft_auth_sign(450,90901,1,(const uint8_t *)cmd,strlen(cmd),
                          key,req,sizeof(req),&len)==0);
    assert(chmod(secret_path,0644)==0);
    assert(raft_auth_read_secret(secret_path,key)!=0);
    assert(chmod(ring,0640)==0);
    assert(raft_auth_read_keyring(ring,&keys)!=0);
    assert(chmod(ring,0600)==0);
    snprintf(text,sizeof(text),"90901 %s\n90901 %s\n",a,b);
    put(ring,text,0600);
    assert(raft_auth_read_keyring(ring,&keys)!=0);
    snprintf(text,sizeof(text),"90901 %s\n",a);
    put(ring,text,0600);
    assert(raft_auth_read_keyring(ring,&keys)==0 && keys.count==1);
    assert(unlink(secret_path)==0);
    assert(symlink(ring,secret_path)==0);
    assert(raft_auth_read_secret(secret_path,key)!=0);
    raft_auth_clear_secret(key);
    raft_auth_clear_keys(&keys);
    assert(unlink(secret_path)==0 && unlink(ring)==0 && rmdir(directory)==0);
    puts("PASS: Phase 25 authenticated client codec, identity binding, tamper checks and private files");
    return 0;
}
