/* sec-sysvc2: SYSTEM jobs and sealed boot settings of call 99 (sysvc.h;
 * abi.h "sec-sysvc2"; user-security-kernel.md, sec-sysvc2 amendment;
 * research and citations in docs/validation/sec-sysvc2.md).
 *
 * Jobs. Windows XP runs scheduled system maintenance as LocalSystem, whoever
 * is logged on (Task Scheduler, System Restore checkpoints, autochk at boot
 * with its result logged by the system). The desktop of every session asks
 * for the start-of-session maintenance here; the kernel does the work as
 * SYSTEM from its own records, so a standard user's or Guest's session gets
 * the same SCANDISK.LOG entries as an administrator's and no caller data
 * reaches a system log. (The daily Registry Checker backup is the registry
 * broker's SYSTEM job: apps/registry/checker.c.)
 *
 * Sealed boot settings. HWPROF.CFG (read by the loader and the kernel),
 * performance.cfg (G30B) and the display retry request accel-retry.cfg (DM8)
 * are consumed as privileged configuration at the next start; the rule table
 * makes them SYSTEM maintenance only (fs/security_policy.c seals them).
 * Administrators change them through SETTING, which validates the text and
 * rewrites the file as SYSTEM, like BOOT_CONFIG. sec-k08 adds clock.cfg (the
 * time zone service's store, read at boot: lead decision S2-5) and the
 * machine power buttons power.cfg (lead decision U4-6).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/abi.h> /* before the C library headers (FS_HOST): its macros win */
#include "sysvc.h"
#include "vfs.h"
#include <ciuki/security.h>
#include <ciuki/storage.h>
#include <ciuki/systime.h>
#include <ciuki/tzrule.h>
#include <ciuki/system_settings.h>
#include <ciuki/ipx_config.h>
#ifdef FS_HOST
#include <stdio.h>
#define SCANDISK_TEXT_VFORMAT vsnprintf
#define job_log(...) do { printf(__VA_ARGS__); putchar('\n'); } while (0)
#define job_format snprintf
#else
#include <ciuki/kernel.h>
#define SCANDISK_TEXT_VFORMAT kvsnprintf
#define job_log(...) klog(__VA_ARGS__)
#define job_format ksnprintf
#endif
#include <ciuki/scandisk_text.h>
/* Weak: the host harnesses link no time zone service (UTC then). */
extern int32_t timezone_offset_at(int64_t) __attribute__((weak));
extern int file_clock_now(uint32_t, const struct process *, struct ciuki_timespec *) __attribute__((weak));

/* ---- jobs ---- */
#define SCANDISK_LOG_LIMIT (256u * 1024u) /* the tools' SR_LOG_LIMIT */
static uint32_t reported;                 /* drives whose at-start report is in the log */

static void local_minute(char out[32])
{
    struct ciuki_timespec now;
    if (!file_clock_now || file_clock_now(CLOCK_REALTIME, 0, &now)) { job_format(out, 32, "?"); return; }
    int64_t local = now.tv_sec + (timezone_offset_at ? timezone_offset_at(now.tv_sec) : 0);
    int64_t days = tzr_floor_div(local, 86400), seconds = local - days * 86400, year;
    unsigned month, day;
    tzr_civil_from_days(days, &year, &month, &day);
    job_format(out, 32, "%04u-%02u-%02u %02u:%02u", (unsigned)year, month, day, (unsigned)(seconds / 3600),
               (unsigned)(seconds % 3600 / 60));
}
/* Every ScanDisk check this start ran, once each, as SYSTEM. */
static int startup_report(uint32_t *done)
{
    struct { struct ciuki_scandisk r; char when[32], text[2048], entry[2304]; } *w = fs_contig_alloc(sizeof(*w));
    if (!w) return -ENOMEM;
    int e = 0;
    for (unsigned d = 0; d < 26 && !e; d++) {
        if ((reported >> d) & 1u) continue;
        if (storage_scandisk_report(d, &w->r) || !(w->r.flags & CIUKI_SCANDISK_RAN)) continue;
        /* Claimed before the write (a filesystem call may yield to another
         * session's request); given back when the append fails. */
        reported |= 1u << d;
        local_minute(w->when);
        size_t n = scandisk_log_entry(&w->r, w->when, w->text, sizeof(w->text), w->entry, sizeof(w->entry));
        uint32_t total = 0;
        e = sysvc_log_system_append("SCANDISK.LOG", w->entry, n, SCANDISK_LOG_LIMIT, &total);
        if (e) reported &= ~(1u << d);
        else (*done)++;
    }
    fs_contig_free(w, sizeof(*w));
    return e;
}
int sysvc_job_run(struct security_subject *s, unsigned job, uint32_t *done)
{
    const struct ciuki_credential *c = s ? security_subject_credential(s) : 0;
    if (!done) return -EINVAL;
    *done = 0;
    if (!c || !security_subject_live(s)) return -EACCES;
    /* A session's desktop (any account, Guest included) or SYSTEM itself;
     * other service subjects have no session to maintain. */
    if (!c->session_id && !sysvc_admin(s)) return -EPERM;
    if (job != CIUKI_SYSTEM_JOB_STARTUP_REPORT) return -EINVAL;
    int e = startup_report(done);
    if (*done || e) job_log("[sysvc] job startup-report uid=%u session=%u appended=%u result=%d", c->uid, c->session_id, *done, e);
    return e;
}

/* ---- sealed boot settings ---- */
static bool lower_hex(const char *p, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) return false;
    return true;
}
/* "driver=<id>\npci=<vvvv>:<dddd>\n": the request attempt.c compares with
 * its own key (display_driver.h DISPLAY_RETRY_PATH). */
static bool retry_request(const char *t, size_t n)
{
    if (n < 7 || memcmp(t, "driver=", 7)) return false;
    size_t id = 7;
    while (id < n && t[id] != '\n') {
        char ch = t[id];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return false;
        id++;
    }
    if (id == 7 || id - 7 > 15 || n != id + 15) return false;
    const char *p = t + id;
    return !memcmp(p, "\npci=", 5) && lower_hex(p + 5, 4) && p[9] == ':' && lower_hex(p + 10, 4) && p[14] == '\n';
}
/* ---- sec-k08: the machine power buttons (lead decision U4-6). Exactly
 * the power_button= and power_lid= lines of apps/desktop/power_policy.c,
 * each at most once, every line ending in LF, nothing else. */
static bool line_is(const char *t, size_t n, const char *key, const char *const *names, unsigned count)
{
    size_t k = strlen(key);
    if (n <= k || memcmp(t, key, k)) return false;
    for (unsigned i = 0; i < count; i++)
        if (n - k == strlen(names[i]) && !memcmp(t + k, names[i], n - k)) return true;
    return false;
}
static bool power_buttons(const char *t, size_t n)
{
    static const char *const button[] = {"shutdown", "standby"}, *const lid[] = {"none", "standby", "shutdown"};
    bool seen_button = false, seen_lid = false;
    size_t at = 0;
    if (!n || n > 127) return false;
    while (at < n) {
        size_t end = at;
        while (end < n && t[end] != '\n') end++;
        if (end == n) return false;
        if (!seen_button && line_is(t + at, end - at, "power_button=", button, 2)) seen_button = true;
        else if (!seen_lid && line_is(t + at, end - at, "power_lid=", lid, 3)) seen_lid = true;
        else return false;
        at = end + 1;
    }
    return true;
}
/* ---- end sec-k08 ---- */
int sysvc_setting_set(struct security_subject *s, unsigned setting, const char *text, size_t length)
{
    const struct ciuki_credential *c = s ? security_subject_credential(s) : 0;
    if (!sysvc_admin(s)) return -EPERM;
    if ((length && !text) || length > CIUKI_SYSTEM_SETTING_MAX) return -EINVAL;
    const char *path = 0; size_t keep = 0;
    struct system_perf perf;
    unsigned ipx=CIUKI_IPX_FRAME_AUTO;
    switch (setting) {
    case CIUKI_SYSTEM_SETTING_HWPROFILES:
        if (!sysvc_printable(text, length)) return -EINVAL;
        path = SYSVC_HWPROFILES; keep = CIUKI_SYSTEM_SETTING_MAX;
        break;
    case CIUKI_SYSTEM_SETTING_PERFORMANCE:
        if (length > 511 || !system_perf_parse(&perf, text, length)) return -EINVAL;
        path = SYSVC_PERFORMANCE; keep = 511;
        break;
    case CIUKI_SYSTEM_SETTING_ACCEL_RETRY:
        if (!retry_request(text, length)) return -EINVAL;
        path = SYSVC_ACCEL_RETRY; keep = 47;
        break;
    /* sec-k08 (S2-5): clock.cfg, read at boot before any process; the
     * whole record must parse and select a rule, as tz_load requires. */
    case CIUKI_SYSTEM_SETTING_CLOCK: {
        struct tzr_config clock; struct tzr rule;
        if (length > 511 || !tzr_config_parse(text, length, &clock) || !tzr_config_rule(&clock, &rule)) return -EINVAL;
        path = SYSVC_CLOCK; keep = 511;
        break;
    }
    case CIUKI_SYSTEM_SETTING_POWER:   /* sec-k08 (U4-6) */
        if (!power_buttons(text, length)) return -EINVAL;
        path = SYSVC_POWER; keep = 127;
        break;
    case CIUKI_SYSTEM_SETTING_IPX_FRAME:
        if (!ipx_config_parse(text,length,&ipx)) return -EINVAL;
        path = "/System/Settings/ipx.cfg"; keep = IPX_CONFIG_BYTES;
        break;
    default: return -EINVAL;
    }
    bool framing=setting==CIUKI_SYSTEM_SETTING_IPX_FRAME;
    if (framing && !net_ipx_config_lock()) return -EBUSY;
    int e = sysvc_system_rewrite(path, text, length, keep);
    if (framing) {
        if (!e) net_ipx_frame_publish(ipx);
        net_ipx_config_unlock();
    }
    job_log("[sysvc] setting saved uid=%u setting=%u bytes=%u result=%d", c ? c->uid : 0, setting, (unsigned)length, e);
    return e;
}
