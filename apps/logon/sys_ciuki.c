/* SPDX-License-Identifier: MIT
 * Target binding of LOGON's lgn_sys seam over the static Ciuki SDK; the
 * transport calls are the shared client binding (client_ciuki.c).
 */
#include "logon.h"
#include "sys_ciuki.h"
#include <ciuki/proc.h>
#include <ciuki/power.h>
#include <ciuki/recovery.h>
#include <errno.h>

static int boot_flags(void *ctx, uint32_t *flags)
{
    (void)ctx;
    struct ciuki_recovery_status s;
    if (ciuki_recovery_status(&s)) return -errno;
    *flags=s.boot_flags; return 0;
}
/* The orderly path (storage flush, then soft-off or reset). REBOOT does not
 * return on success; OFF may return ENOTSUP/EIO after storage stopped. */
static int power(void *ctx, unsigned action)
{
    (void)ctx;
    return ciuki_power(action==LGN_EVENT_RESTART ? CIUKI_POWER_REBOOT : CIUKI_POWER_OFF) ? -errno : 0;
}
static struct { struct ciuki_proc_system system; struct ciuki_proc_record records[CIUKI_PROCESS_MAX]; } snapshot;
static int pids(void *ctx, uint32_t *out, unsigned capacity)
{
    (void)ctx;
    int n=ciuki_proc_list(&snapshot,sizeof(snapshot));
    if (n<0) return -errno;
    unsigned count=0;
    for (int i=0;i<n && count<capacity;i++)
        if (snapshot.records[i].state==CIUKI_PROC_LIVE) out[count++]=snapshot.records[i].pid;
    return (int)count;
}
static void log_line(void *ctx, const char *line) { (void)ctx; lgn_log_ciuki(line); }
/* sec-k08 (U4-3): firmware resume and critical suspend counts (call 73). */
static int resumes(void *ctx, uint32_t *count)
{
    (void)ctx; struct ciuki_power_events e;
    if (ciuki_power_events(&e)) return -errno;
    *count=e.resumes+e.critical; return 0;
}

const struct lgn_sys lgn_sys_ciuki={NULL,lgn_ciuki_security,lgn_ciuki_send,lgn_ciuki_recv,lgn_ciuki_close,lgn_ciuki_wait,
                                    lgn_ciuki_now,boot_flags,power,pids,log_line,resumes};
