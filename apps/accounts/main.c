/* SPDX-License-Identifier: MIT
 * Sealed SYSTEM account service image (/System/Services/accounts). PID 1
 * spawns it from the fixed boot manifest with the ACCOUNT role (sec-k01); it
 * serves the AUTH (1) and ACCOUNTS (2) endpoints over the call-101 service
 * links and the private CSC1 framing (server.c), with the production kernel
 * adapter, the interim OWNER backend and protected storage under
 * /System/Security. Without bootstrap evidence or a real OWNER the service
 * stays closed and answers every request with EIO. It never exits on its
 * own: PID 1 would respawn it, so a binding failure idles instead.
 */
#include "kernel_sys.h"
#include "production.h"
#include "server.h"
#include <string.h>

#define IDLE_WAIT_MS 250u
#define STORE_DIRECTORY "/System/Security"

static struct acct_server server;
static struct acct_posix_io io_state;

int main(void)
{
    struct acct_store tables; memset(&tables,0,sizeof(tables));
    acct_owner_interim(&tables.owner);
    tables.volume=ACCT_INTERIM_VOLUME;
    if (acct_io_posix(&io_state,STORE_DIRECTORY,&tables.io) || acct_server_init(&server,&tables,0)) {
        acct_sys_log("[accounts] binding failed; idle");
        for (;;) acct_sys_wait_readable(NULL,0,60000u);
    }
    acct_sys_log("[accounts] sealed service starting; OWNER interim (publication refused)");
    for (;;) acct_server_step(&server,IDLE_WAIT_MS);
}
