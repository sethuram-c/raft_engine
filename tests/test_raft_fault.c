#define _POSIX_C_SOURCE 200809L
#include "raft_fault.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)

static void write_rules(const char *path, const char *contents)
{
    FILE *f = fopen(path,"wb");
    CHECK(f != NULL);
    CHECK(fwrite(contents,1,strlen(contents),f)==strlen(contents));
    CHECK(fclose(f)==0);
}

int main(void)
{
    char directory[] = "/tmp/raft-fault-test-XXXXXX";
    CHECK(mkdtemp(directory)!=NULL);
    char path[512];
    CHECK(snprintf(path,sizeof(path),"%s/node-1.block",directory)>0);
    raft_fault_policy_t p={0};
    CHECK(raft_fault_init(&p,1,directory)==0);
    CHECK(p.enabled==1 && p.self_id==1);
    CHECK(raft_fault_blocks(&p,2)==0); /* absent means allow */
    CHECK(raft_fault_blocks(&p,3)==0);
    CHECK(raft_fault_blocks(&p,1)==0);
    write_rules(path,"2\n");
    CHECK(raft_fault_blocks(&p,2)==1 && raft_fault_blocks(&p,3)==0);
    write_rules(path," 2   3\t\n");
    CHECK(raft_fault_blocks(&p,2)==1 && raft_fault_blocks(&p,3)==1);
    write_rules(path,"");
    CHECK(raft_fault_blocks(&p,2)==0 && raft_fault_blocks(&p,3)==0);
    write_rules(path,"4294967295\n");
    CHECK(raft_fault_blocks(&p,2)==0);
    const char *invalid[]={"0","2;3","2x","-3","4294967296", "2\r,3", "\001", "18446744073709551616"};
    for (size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
        write_rules(path,invalid[i]);
        CHECK(raft_fault_blocks(&p,2)==1);
        CHECK(raft_fault_blocks(&p,3)==1);
    }
    char excessive[RAFT_FAULT_RULE_MAX+10];
    memset(excessive,' ',sizeof(excessive)-1); excessive[sizeof(excessive)-1]='\0';
    write_rules(path,excessive);
    CHECK(raft_fault_blocks(&p,2)==1);
    CHECK(unlink(path)==0);
    CHECK(symlink("/dev/null",path)==0);
    CHECK(raft_fault_blocks(&p,2)==1); /* no following attacker link */
    CHECK(unlink(path)==0);
    write_rules(path,"3");
    CHECK(chmod(path,0666)==0);
    CHECK(raft_fault_blocks(&p,2)==1);
    CHECK(chmod(path,0600)==0);
    CHECK(raft_fault_blocks(&p,2)==0 && raft_fault_blocks(&p,3)==1);
    CHECK(raft_fault_init(NULL,1,directory)!=0);
    CHECK(raft_fault_init(&p,0,directory)!=0);
    CHECK(raft_fault_init(&p,1,NULL)!=0);
    CHECK(chmod(directory,0755)==0);
    CHECK(raft_fault_init(&p,1,directory)!=0);
    CHECK(chmod(directory,0700)==0);
    CHECK(unlink(path)==0);
    char moved[sizeof(directory) + 16];
    CHECK(snprintf(moved,sizeof(moved),"%s-moved",directory)>0);
    CHECK(rename(directory,moved)==0);
    CHECK(raft_fault_blocks(&p,2)==1); /* missing directory != missing rule */
    CHECK(rename(moved,directory)==0);
    CHECK(raft_fault_blocks(&p,2)==0); /* private directory restored */
    CHECK(rmdir(directory)==0);
    printf("PASS: %u Phase 30 private-policy, parsing, fail-closed and allow rules\n",checks);
    return 0;
}
