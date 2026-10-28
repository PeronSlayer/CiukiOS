/* SPDX-License-Identifier: MIT
 * Production desktop LOGON transport, bound only at the kernel syscall seam.
 * The session id normally comes from LOGON's kernel-generated launch labels.
 */
#include "../desktop/replay_base.h"
#include "../../../apps/desktop/logon_session.c"

void cpt_desktop_bind(const struct lgn_client_sys *sys,uint32_t session)
{
    session_id=session;
    lgn_client_init(&logon_link,sys,LGN_SERVICE_LOGON,LGN_ROLE_LOGON);
}
void cpt_desktop_unbind(void)
{
    lgn_client_close(&logon_link);
}
