/* SPDX-License-Identifier: MIT
 * Sealed SYSTEM LOGON service image (/System/Services/logon). PID 1 spawns
 * it from the fixed boot manifest with the LOGON role (sec-k01) and respawns
 * it with a fresh role generation; it never exits on its own. Its presence
 * replaces the interim uid-1000 desktop: the trusted gate decides every
 * session (ready images log on automatically as password-less CiukiUser).
 */
#include "logon.h"
#include "sys_ciuki.h"
#include <string.h>

#define IDLE_WAIT_MS 250u

static struct lgn coordinator;

int main(void)
{
    if (lgn_init(&coordinator,&lgn_sys_ciuki)) {
        lgn_log_ciuki("[logon] binding failed; idle");
        for (;;) lgn_sys_ciuki.wait(NULL,NULL,0,60000u);
    }
    lgn_note(&coordinator,"[logon] trusted logon starting safe=%d",coordinator.safe);
    for (;;) lgn_step(&coordinator,IDLE_WAIT_MS);
}
