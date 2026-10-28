/* Sealed /System/Services/registry. The desktop and w32srv remain users.
 * SPDX-License-Identifier: MIT */
#include "server.h"
#include "restart_store.h"
#include <string.h>
#include "../accounts/kernel_sys.h"
#include <sys/stat.h>
static struct rb_server server;
static struct wr_server registry;
int main(void)
{
    const char *dirs[]={"/System/Security/Registry","/System/Security/Registry/Machine","/System/Security/Restart"};
    for(unsigned i=0;i<3;i++)(void)mkdir(dirs[i],0700);
    struct rb_storage storage;rb_storage_posix(&storage);storage.users=rb_account_users;struct wr_io io;wr_posix_io(&io,"/System/Security/Registry/Machine");
    for(;;){
        if(wr_init_secure(&registry,&io,acct_sys_monotonic_ms())&&
           !rb_server_init(&server,&storage,&registry,rb_account_current,NULL))break;
        wr_destroy(&registry);acct_sys_log("[registry] service unavailable; startup refused");acct_sys_wait_readable(NULL,0,30000);
    }
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
        }
    }
}
