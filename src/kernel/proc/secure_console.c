/* sec-k07: the real backend of the k01 secure-console coordinator.
 *
 * Research (recorded with links in docs/validation/sec-k07.md):
 * - Microsoft "Initializing Winlogon" / "Responsibilities of Winlogon":
 *   Winlogon registers Ctrl+Alt+Delete as the secure attention sequence
 *   before other processes can hook it and switches WinSta0 to its own
 *   Winlogon desktop, so logon/unlock UI is invisible to applications.
 * - WlxLoggedOnSAS / WlxWkstaLockedSAS: SAS while logged on opens the
 *   security options; SAS while locked leads to the unlock dialog only.
 * - "Logging Off" / ExitWindowsEx(EWX_LOGOFF): every process of the logon
 *   session is shut down before the user is logged off.
 * CiukiOS keeps the frozen call-101 ABI: security.c owns states, epochs and
 * cookies; this file only performs nonblocking device-domain transitions,
 * the deferred physical attention, the neutral frame and session drain.
 *
 * Descriptor layout (SESSION_LAUNCH): 0..2 and 3 are one private session
 * console (writes to the session log, reads EOF); the desktop gets a
 * suspended display/input pair, w32srv a kernel notification channel.
 * LOGON_UI gets the same console plus the trusted display/input pair.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/security.h>
#include <ciuki/desktop.h>
#include <ciuki/files.h>

extern int files_session_console(struct process *, int32_t *) __attribute__((weak));
extern unsigned dosvm_session_revoke(uint32_t) __attribute__((weak));
extern unsigned w32_session_revoke(uint32_t) __attribute__((weak));
/* sec-user-04: a closing session's joystick preferences return to the
 * machine store (sec-user-05's per-user scope, drivers/joystick_settings.c). */
extern void joystick_settings_session_end(uint32_t) __attribute__((weak));
#pragma weak channel_kernel_endpoint
#pragma weak channel_kernel_notify

struct secure_record {
    uint32_t session, grant_generation, retained;
    bool drained;
    struct proc_object *service; /* kernel end of the w32srv notification */
};
static struct secure_record records[CIUKI_SECURITY_SESSION_MAX];
static uint32_t attention_pending;
static int attention_result;
static uint64_t attention_handled, drain_passes;

static struct secure_record *record_for(uint32_t session, bool create)
{
    struct secure_record *free_slot = 0;
    if (!session) return 0;
    for (unsigned i = 0; i < ARRAY_SIZE(records); i++) {
        if (records[i].session == session) return &records[i];
        if (!records[i].session && !free_slot) free_slot = &records[i];
    }
    if (!create || !free_slot) return 0;
    *free_slot = (struct secure_record){ .session = session };
    return free_slot;
}

static int k07_prepare(struct process *child, uint32_t purpose, struct ciuki_security_launch *l)
{
    const struct ciuki_credential *c = security_credential(child);
    if (!child || child->published || child->state != PROC_PREPARING || !c || !c->session_id ||
        c->session_id != l->session_id) return -EACCES;
    if (purpose != CIUKI_SECURITY_LAUNCH_DESKTOP && purpose != CIUKI_SECURITY_LAUNCH_W32SRV) return -EINVAL;
    struct secure_record *r = record_for(c->session_id, true);
    if (!r) return -EAGAIN;
    int32_t console = -1;
    int err = files_session_console ? files_session_console(child, &console) : -EIO;
    if (err) return err;
    l->child_console_fd = console;
    if (purpose == CIUKI_SECURITY_LAUNCH_DESKTOP) {
        int32_t fds[2];
        err = grants_install_session(child, c->session_id, fds);
        if (!err) { l->child_display_fd = fds[0]; l->child_input_fd = fds[1]; }
        return err;
    }
    struct proc_object *end = 0;
    int32_t fd = -1;
    err = channel_kernel_endpoint ? channel_kernel_endpoint(child, &fd, &end) : -EIO;
    if (err) return err;
    if (r->service) r->service->release(r->service);
    r->service = end;
    l->child_service_fd = fd;
    return 0;
}
static int k07_conceal(uint32_t session, uint32_t grant_generation)
{
    struct secure_record *r = record_for(session, false);
    if (r && grant_generation > r->grant_generation) r->grant_generation = grant_generation;
    return grants_console_conceal(session);
}
static int k07_activate(uint32_t session, uint32_t grant_generation)
{
    struct secure_record *r = record_for(session, false);
    if (!r) return -ESRCH;
    if (grant_generation <= r->grant_generation) return -EAGAIN; /* fresh lease generation only */
    int err = grants_console_activate(session);
    if (!err) r->grant_generation = grant_generation;
    return err;
}
static void k07_release(uint32_t session)
{
    grants_console_release(session);
    struct secure_record *r = record_for(session, false);
    if (!r) return;
    struct proc_object *service = r->service;
    *r = (struct secure_record){ 0 };
    if (service) service->release(service);
}
static int k07_trusted(struct process *p)
{
    int32_t console = -1, fds[2];
    int err = files_session_console ? files_session_console(p, &console) : -EIO;
    return err ? err : grants_install_trusted(p, fds);
}
const struct security_grant_ops secure_console_ops = {
    k07_prepare, k07_conceal, k07_activate, k07_release, k07_trusted
};

/* Producer side (merged input queue). Armed only once a kernel LOGON role
 * has been registered (sticky for the boot); the interim desktop keeps its
 * Task Manager behaviour. Never blocks: PID 1 performs the transition. */
bool security_attention_armed(void) { return security_roles_active(); }
void security_attention_physical(void)
{
    uint32_t f = irq_save();
    if (attention_pending != UINT32_MAX) attention_pending++;
    irq_restore(f);
    proc_notify();
}
/* sec-k08 (lead decision U4-3): a firmware-initiated suspend or resume
 * (core/power_source.c) conceals the session through the same deferred
 * entry as Ctrl+Alt+Delete; LOGON's watch sees the moved gate and the call-73
 * resume count, then locks (an owner with a password) or restores. Armed
 * only once LOGON is registered; the interim desktop keeps its own path. */
static uint32_t resume_pending;
static uint64_t resume_handled;
void security_attention_resume(void)
{
    if (!security_attention_armed()) return;
    uint32_t f = irq_save();
    if (resume_pending != UINT32_MAX) resume_pending++;
    irq_restore(f);
    proc_notify();
}
/* COLLECT keeps a session CLOSING until a PID 1 pass found no DOS VM or
 * Win32 session object left. Nonblocking; registers unknown sessions. */
unsigned secure_session_retained(uint32_t session)
{
    struct secure_record *r = record_for(session, true);
    if (!r) return 1;
    if (r->drained) return 0;
    proc_notify();
    return r->retained ? r->retained : 1;
}
void secure_service_notify(uint32_t session)
{
    struct secure_record *r = record_for(session, false);
    if (!r || !r->service || !channel_kernel_notify) return;
    const uint32_t words[4] = { 1, 1, session, 0 }; /* version, ACCEPT_PENDING, session */
    (void)channel_kernel_notify(r->service, words, sizeof(words));
}
void secure_console_stats(struct secure_console_stats *out)
{
    uint32_t f = irq_save();
    *out = (struct secure_console_stats){ .attention_pending = attention_pending,
        .attention_handled = attention_handled, .drain_passes = drain_passes,
        .attention_result = attention_result };
    irq_restore(f);
}
/* PID 1 only (supervisor_poll): may yield, never under a security lock. */
void secure_console_poll(void)
{
    uint32_t f = irq_save();
    uint32_t pending = attention_pending;
    attention_pending = 0;
    irq_restore(f);
    if (pending) {
        attention_result = security_console_attention(CIUKI_SECURITY_SECURE_ATTENTION);
        attention_handled++;
        klog("[security] secure attention source=keyboard result=%d", attention_result);
    }
    f = irq_save(); pending = resume_pending; resume_pending = 0; irq_restore(f);   /* sec-k08 */
    if (pending) {
        int r = security_console_attention(CIUKI_SECURITY_SECURE_ATTENTION);
        resume_handled++;
        klog("[security] secure attention source=resume result=%d handled=%llu", r, (unsigned long long)resume_handled);
    }
    (void)grants_console_neutral();
    for (unsigned i = 0; i < ARRAY_SIZE(records); i++) {
        uint32_t session = records[i].session, state = 0;
        if (!session || records[i].drained) continue;
        if (security_session_query(proc_supervisor(), session, &state) ||
            state != CIUKI_SECURITY_SESSION_CLOSING) continue;
        unsigned retained = 0;
        if (dosvm_session_revoke) retained += dosvm_session_revoke(session);
        if (w32_session_revoke) retained += w32_session_revoke(session);
        if (joystick_settings_session_end) joystick_settings_session_end(session); /* sec-user-04 */
        drain_passes++;
        /* The record may have been released while the drain yielded. */
        if (records[i].session != session) continue;
        records[i].retained = retained;
        records[i].drained = !retained;
        if (!retained) klog("[security] session drained session=%u passes=%llu", session, drain_passes);
    }
}
