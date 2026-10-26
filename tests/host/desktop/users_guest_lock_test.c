/* Production __CIUKIOS__ lock path; only the service and process snapshot are
 * injected. SPDX-License-Identifier: MIT */
#include "replay_base.h"
#include "../../../sdk/sysroot-overlay/include/ciuki/proc.h"
#include "../../../sdk/sysroot-overlay/include/ciuki/recovery.h"
#include "../../../sdk/win32/include/w32/server.h"
#include "users.h"
#include <assert.h>
static unsigned held,acquires,releases,boot=100,live_pid=9999;
static int snapshot_error,transport_error;
int ciuki_recovery_status(struct ciuki_recovery_status *r)
{ memset(r,0,sizeof(*r)); r->flags=CIUKI_RECOVERY_VALID;r->boots=boot;return 0; }
int ciuki_proc_list(void *buffer,uint32_t bytes)
{
    assert(bytes>=sizeof(struct ciuki_proc_system)+sizeof(struct ciuki_proc_record));
    if(snapshot_error)return -1;
    struct ciuki_proc_system *s=buffer;s->records=live_pid ? 1 : 0;
    struct ciuki_proc_record *r=(void *)(s+1);r->pid=live_pid;r->state=CIUKI_PROC_LIVE;
    return (int)s->records;
}
uint32_t desk_w32_exchange(const void *wire,uint32_t n,void *out,uint32_t cap,uint32_t *used)
{
    const struct w32r_request *q=wire;
    assert(n==sizeof(*q)+2 && cap==sizeof(struct w32r_reply));
    assert(q->path_bytes==1 && q->name_bytes==1 && !q->key && !q->flags && !q->access);
    if(transport_error)return 1062;
    struct w32r_reply r={.version=W32R_VERSION};
    if(q->op==W32R_PROFILE_ACQUIRE){acquires++;if(held)r.error=170;else held=1;}
    else {assert(q->op==W32R_PROFILE_RELEASE && held);held=0;releases++;}
    memcpy(out,&r,sizeof(r));*used=sizeof(r);return 0;
}
static void leftover(const char *path,unsigned pid,unsigned old_boot)
{ FILE *f=fopen(path,"wb");assert(f);fprintf(f,"CUSR1 %u boot-%u\n",pid,old_boot);assert(!fclose(f)); }
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);
    assert(argc==2);struct users_request q={.operation=USERS_POLICY};struct users_view v;
    assert(!users_execute(argv[1],&q,&v) && !held && acquires==1 && releases==1);
    char path[512];snprintf(path,sizeof(path),"%s/system/settings/users.lock",argv[1]);
    leftover(path,live_pid,99);assert(!users_execute(argv[1],&q,&v)); /* old boot, even a live reused PID */
    leftover(path,9998,100);assert(!users_execute(argv[1],&q,&v)); /* dead PID */
    leftover(path,live_pid,100);assert(users_execute(argv[1],&q,&v)==-EBUSY && !held && !access(path,F_OK));
    snapshot_error=1;assert(users_execute(argv[1],&q,&v)==-EIO && !held && !access(path,F_OK));snapshot_error=0;
    held=1;unsigned released=releases;assert(users_execute(argv[1],&q,&v)==-EBUSY && held && releases==released && !access(path,F_OK));held=0;
    transport_error=1;assert(users_execute(argv[1],&q,&v)==-EIO && !access(path,F_OK));transport_error=0;
    live_pid=0;assert(!users_execute(argv[1],&q,&v) && !held && access(path,F_OK));
    boot=0;assert(users_execute(argv[1],&q,&v)==-EIO && !held);boot=100;
    unsigned before=acquires;q.operation=USERS_READ;assert(!users_execute(argv[1],&q,&v) && acquires==before);
    puts("PASS g32b guest lock: old boot, dead PID, live contention, recovery serialization, service/snapshot/boot failure, read without lease");
}
