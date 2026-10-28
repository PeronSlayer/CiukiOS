/* Sealed /System/Services/registry. The desktop and w32srv remain users.
 * SPDX-License-Identifier: MIT */
#include "server.h"
#include "restart_store.h"
#include <string.h>
#include "../accounts/kernel_sys.h"
#include <sys/stat.h>
/* ---- sec-sysvc2: Registry Checker (checker.c) with this start's status and
 * the system clock; its boot work runs before any hive or root is loaded. */
#include "checker.h"
#include <ciuki/recovery.h>
#include <ciuki/timezone.h>
#include <time.h>
static int checker_started(void *p)
{ (void)p; struct ciuki_recovery_status s; return !ciuki_recovery_status(&s) && (s.flags & CIUKI_RECOVERY_COMPLETE) ? 1 : 0; }
static int64_t checker_now(void *p,int *offset)
{
    (void)p; struct ciuki_time_zone z; memset(&z,0,sizeof(z));
    if(!ciuki_time_zone(&z,CIUKI_TIME_GET)){*offset=z.offset_minutes;return z.utc_seconds;}
    *offset=0;return (int64_t)time(NULL);
}
static void checker_log(void *p,const char *line){(void)p;acct_sys_log(line);}
static struct rb_checker checker={.root="",.started=checker_started,.now=checker_now,.log=checker_log};
/* ---- end sec-sysvc2 ---- */
static struct rb_server server;
static struct wr_server registry;
int main(void)
{
    const char *dirs[]={"/System/Security/Registry","/System/Security/Registry/Machine","/System/Security/Restart"};
    for(unsigned i=0;i<3;i++)(void)mkdir(dirs[i],0700);
    rb_checker_boot(&checker); /* sec-sysvc2 */
    struct rb_storage storage;rb_storage_posix(&storage);storage.users=rb_account_users;struct wr_io io;wr_posix_io(&io,"/System/Security/Registry/Machine");
    for(;;){
        if(wr_init_secure(&registry,&io,acct_sys_monotonic_ms())&&
           !rb_server_init(&server,&storage,&registry,rb_account_current,NULL))break;
        wr_destroy(&registry);acct_sys_log("[registry] service unavailable; startup refused");acct_sys_wait_readable(NULL,0,30000);
    }
    server.broker.checker=&checker; /* sec-sysvc2 */
    struct rj_posix evidence_directory;struct rj_io evidence_io;int quarantined=0;uint64_t retry=0;
    for(;;){
        if(!quarantined&&acct_sys_monotonic_ms()>=retry){
            struct rb_identity install;retry=acct_sys_monotonic_ms()+1000;
            if(!rb_installation_identity(&install)&&
               !rj_posix_io(&evidence_directory,"/System/Security/Restart/Legacy",&evidence_io)){
                struct rj_quarantine q={.evidence=evidence_io,.source=rj_posix_legacy,.volume=install.volume};
                memcpy(q.sid,install.machine_sid,sizeof(q.sid));quarantined=!rj_quarantine_run(&q);
            }
        }
        rb_server_step(&server,20);
        if(server.broker.faulted){
            rb_server_destroy(&server);wr_destroy(&registry);
            for(;;){acct_sys_wait_readable(NULL,0,1000);
                if(wr_init_secure(&registry,&io,acct_sys_monotonic_ms())&&
                   !rb_server_init(&server,&storage,&registry,rb_account_current,NULL))break;
                wr_destroy(&registry);
            }
            server.broker.checker=&checker; /* sec-sysvc2 */
        }
    }
}
