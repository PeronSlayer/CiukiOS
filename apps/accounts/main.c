/* SPDX-License-Identifier: MIT
 * Sealed SYSTEM account service image (/System/Services/accounts). PID 1
 * spawns it from the fixed boot manifest with the ACCOUNT role (sec-k01); it
 * serves the AUTH (1) and ACCOUNTS (2) endpoints over the call-101 service
 * links and the private CSC1 framing (server.c), with the production kernel
 * adapter, the kernel OWNER adapter (owner_kernel.c, sec-own) and protected
 * storage under /System/Security. The system volume identity comes from the
 * kernel OWNER (the same partition LBA << 32 | FAT serial value the builder
 * and Setup stage). Without bootstrap evidence or a usable OWNER store the
 * service stays closed and answers every request with EIO. It never exits on
 * its own: PID 1 would respawn it, so a binding failure idles instead.
 */
#include "kernel_sys.h"
#include "production.h"
#include "server.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define IDLE_WAIT_MS 250u
#define IDENTITY_RETRY_MS 1000u
#define IDENTITY_ATTEMPTS 5u
#define STORE_DIRECTORY "/System/Security"

static struct acct_server server;
static struct acct_posix_io io_state;
static struct acct_legacy_io legacy_io; /* sec-user-02: G32 settings for first-boot migration */
static struct acct_owner_link owner_link; /* sec-own */
static struct acct_tree_io tree_io;       /* sec-own */

static void idle(const char *why, int code)
{
    char line[128]; snprintf(line,sizeof(line),"[accounts] %s: %d; idle",why,code);
    acct_sys_log(line);
    for (;;) acct_sys_wait_readable(NULL,0,60000u);
}

int main(void)
{
    struct acct_store tables; memset(&tables,0,sizeof(tables));
    if (acct_tree_posix(&tree_io) || acct_owner_kernel(&owner_link,&tree_io,&tables.owner))
        idle("binding failed",-EINVAL);
    /* sec-own: one volume identity shared with OWNER, Setup and the builder.
     * Without it (no managed system volume, OWNER unreachable) the service
     * still serves its endpoints and answers every request with EIO at once:
     * the placeholder identity matches no bootstrap evidence. */
    uint64_t volume=0; uint32_t store_open=0; int e=-EIO;
    for (unsigned attempt=0;attempt<IDENTITY_ATTEMPTS;attempt++) {
        e=acct_owner_kernel_identity(&owner_link,&volume,&store_open);
        if (!e || e==-ENODEV) break;
        acct_sys_wait_readable(NULL,0,IDENTITY_RETRY_MS);
    }
    if (e) {
        char line[96]; snprintf(line,sizeof(line),"[accounts] OWNER identity unavailable: %d; service stays closed",e);
        acct_sys_log(line);
        volume=ACCT_INTERIM_VOLUME;
    }
    tables.volume=volume;
    if (acct_io_posix(&io_state,STORE_DIRECTORY,&tables.io) || acct_server_init(&server,&tables,0) ||
        acct_legacy_io_posix(&legacy_io)) idle("binding failed",-EINVAL);
    server.legacy=&legacy_io; /* sec-user-02 first-boot adapter */
    server.tree=&tree_io;     /* sec-own: DOS saves migration */
    char line[128];
    snprintf(line,sizeof(line),"[accounts] sealed service starting; OWNER kernel volume=%016llx store=%s",
             (unsigned long long)volume,e?"none":store_open==1?"open":store_open==2?"unavailable":"unprovisioned");
    acct_sys_log(line);
    for (;;) acct_server_step(&server,IDLE_WAIT_MS);
}
