/* Device authority is credential based; resource leases remain separate.
 * Internal kernel helpers, not a public SDK. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_SECURITY_DEVICE_H
#define CIUKI_SECURITY_DEVICE_H
#include <ciuki/security.h>
#include <ciuki/process.h>
#include <ciuki/supervisor.h>

static inline bool security_device_admin(const struct process *p)
{ return security_process_live(p) && security_admin(p); }
static inline bool security_device_system(const struct process *p)
{ return security_process_live(p) && security_system(p); }
static inline bool security_device_setup(const struct process *p)
{
    return security_device_system(p) && p->setup_capability &&
        security_role(p) == CIUKI_SECURITY_ROLE_SETUP;
}
static inline bool security_device_session(const struct process *p)
{
    const struct ciuki_credential *c = security_credential(p);
    return security_process_live(p) && c && c->session_id && (c->flags & CIUKI_CRED_INTERACTIVE);
}
static inline bool security_device_desktop(const struct process *p)
{
    if (!security_device_session(p)) return false;
    if (security_role(p) == CIUKI_SECURITY_ROLE_DESKTOP)
        return security_session_desktop(security_credential(p)->session_id) == p;
    /* The k01 interim desktop has a credential but no registered role. */
    return !security_roles_active() && p->pid == supervisor_desktop_pid();
}
#endif
