/* SPDX-License-Identifier: MIT
 * G14 desktop side of the startup recovery (docs/design/startup-recovery.md
 * sections 2.3-2.5): Safe mode's corner labels and notice, the Command
 * prompt only shell (one maximized Terminal, reopened when it exits, WIN
 * leaves it), the notices of this start (a failed previous start, drivers
 * the Automatic Skip Driver agent skipped, ScanDisk results) and Registry
 * Checker (a damaged root restored, the daily backup). */
#include "desktop.h"
#include "shell.h"
#include "../sysrecovery/recovery.h"
#include <ciuki/recovery.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define NOTICES 6
struct notice { char title[48], text[320]; int icon, dialog; };
static struct notice queue[NOTICES];
static unsigned queued, shown;
static uint64_t terminal_launch_ns;
static int terminal_zoomed_pid;

static void notice(const char *title, const char *text, int icon, int dialog)
{
    if (queued == NOTICES) return;
    struct notice *n = &queue[queued++];
    snprintf(n->title, sizeof(n->title), "%s", title);
    snprintf(n->text, sizeof(n->text), "%s", text);
    n->icon = icon; n->dialog = dialog;
}

void desk_recovery_init(struct desktop *d, int safe, int terminal)
{
    d->safe_mode = safe; d->terminal_shell = terminal;
}

static void class_list(uint8_t classes, char *out, size_t cap)
{
    static const char *const names[7] = { "the accelerated display driver", "the sound drivers", "the network drivers",
        "the USB drivers", "the serial, parallel, floppy and game ports", "power management", "the startup programs" };
    out[0] = 0;
    for (unsigned c = 0; c < 7; c++) if (classes & (1u << c)) {
        size_t n = strlen(out);
        snprintf(out + n, cap - n, "%s%s", n ? ", " : "", names[c]);
    }
}
/* The summary paragraph only: the dialog holds 320 characters. */
static void scandisk_notice(const struct ciuki_scandisk *r)
{
    char text[2048], title[48];
    scandisk_text(r, text, sizeof(text));
    char *end = strstr(text, "\n\n");
    if (end) *end = 0;
    snprintf(title, sizeof(title), "ScanDisk Results - (%c:)", 'A' + (int)(r->drive < 26 ? r->drive : 25));
    char body[320];
    snprintf(body, sizeof(body), "%.230s\nThe report is in C:\\SYSTEM\\LOGS\\SCANDISK.LOG.", text);
    notice(title, body, r->flags & CIUKI_SCANDISK_ERRORS ? DU_DLG_ICON_WARNING : DU_DLG_ICON_INFO,
           (r->flags & CIUKI_SCANDISK_ERRORS) != 0);
}

void desk_recovery_start(struct desktop *d)
{
    struct ciuki_recovery_status s;
    int have = !ciuki_recovery_status(&s);
    char text[320], list[256];
    if (d->safe_mode)
        notice("Safe mode", "CiukiOS is running in safe mode: only the basic drivers are loaded. Sound, network, "
               "USB and the accelerated display are off. To start normally, restart the computer.",
               DU_DLG_ICON_INFO, 1);
    if (d->terminal_shell)
        notice("Command prompt only", "Only the Terminal runs. Type WIN to start the desktop, SCANREG /RESTORE to "
               "restore the registry or SCANDISK to check a drive.", DU_DLG_ICON_INFO, 0);
    if (have && (s.boot_flags & CIUKI_BOOT_FAILED) && !d->safe_mode)
        notice("CiukiOS", "CiukiOS did not start correctly the last time. If this happens again, hold Ctrl "
               "(or press F8) while the computer starts and choose Safe mode.", DU_DLG_ICON_WARNING, 0);
    if (have && s.newly_skipped) {
        class_list(s.newly_skipped, list, sizeof(list));
        snprintf(text, sizeof(text), "Automatic Skip Driver: %s stopped CiukiOS from starting twice and %s skipped. "
                 "Use System Configuration to enable it again.", list, s.newly_skipped & (s.newly_skipped - 1) ? "were" : "was");
        notice("Automatic Skip Driver", text, DU_DLG_ICON_WARNING, 1);
    }
    /* sec-sysvc2: the at-start ScanDisk reports go to SCANDISK.LOG as a
     * SYSTEM job, whoever logs on (XP logs autochk's result as the system,
     * not as the user); the kernel writes them once from its own records.
     * An older kernel (ENOSYS) or no managed system volume (ENODEV): the
     * desktop appends them as before. */
    uint32_t appended = 0;
    int job = ciuki_system_job(CIUKI_SYSTEM_JOB_STARTUP_REPORT, &appended) ? errno : 0;
    for (unsigned drive = 2; drive < 26; drive++) {
        struct ciuki_scandisk r;
        if (ciuki_scandisk_report(drive, &r) || !(r.flags & CIUKI_SCANDISK_RAN)) continue;
        if (job == ENOSYS || job == ENODEV) (void)scandisk_log("", &r, (int64_t)time(NULL));
        scandisk_notice(&r);
    }
    /* Registry Checker, as ScanRegW at every start. sec-sysvc2: the SYSTEM
     * registry broker restored a damaged root (or a backup an administrator
     * chose) before anything used the registry; the backup of the day is its
     * SYSTEM job, asked for by every session (rc_daily). Not in Command
     * prompt only, where SCANREG is the user's tool. */
    if (!d->terminal_shell && !d->sh.gate) {
        struct rc_status st;
        int known = !rc_status("", &st);
        if (known && st.restored) {
            snprintf(text, sizeof(text), "Registry Checker found an error in the system registry and restored "
                     "it from the backup %s.", st.from);
            notice("Registry Checker", text, DU_DLG_ICON_WARNING, 1);
        }
        if (known && st.applied[0]) {
            snprintf(text, sizeof(text), "Registry Checker restored the registry from the backup %s.", st.applied);
            notice("Registry Checker", text, DU_DLG_ICON_INFO, 0);
        }
        int made = 0, offset = 0;
        time_t now = time(NULL);
        struct tm local, utc;
        if (localtime_r(&now, &local) && gmtime_r(&now, &utc))
            offset = (int)((mktime(&local) - mktime(&utc)) / 60);
        (void)rc_daily("", (int64_t)now, offset, &made);
    }
}

static int terminal_client(const struct desktop *d)
{
    for (int i = 0; i < DESK_CLIENTS; i++)
        if ((d->clients[i].window || d->clients[i].spawn_pending) && d->clients[i].program == DESK_APP_TERMINAL &&
            !d->clients[i].closing) return i;
    return -1;
}
void desk_recovery_tick(struct desktop *d)
{
    if (shown < queued) {
        int busy = d->sh.notification.active;
        for (int i = 0; i < DESK_CLIENTS; i++) if (d->clients[i].window && d->clients[i].internal == DESK_INTERNAL_MESSAGE) busy = 1;
        if (!busy) {
            struct notice *n = &queue[shown++];
            if (n->dialog) desk_shell_message_class(d, n->title, n->text, n->icon);
            else desk_shell_notify(d, n->title, n->text, n->icon, SH_NOTIFY_DETAILS);
        }
    }
    if (!d->terminal_shell || d->power.state) return;
    int i = terminal_client(d);
    if (i < 0) {
        /* The command prompt never disappears: reopen it (at most every 2 s). */
        if (terminal_launch_ns && d->now_ns - terminal_launch_ns < 2000000000ull) return;
        terminal_launch_ns = d->now_ns ? d->now_ns : 1;
        (void)desk_task_launch(d, "terminal", NULL);
        return;
    }
    struct desk_client *c = &d->clients[i];
    if (c->window && !c->zoomed && c->resizable && (int)c->pid != terminal_zoomed_pid) {
        terminal_zoomed_pid = (int)c->pid;
        desk_zoom(d, i);
    }
}
int desk_recovery_startup_allowed(const struct desktop *d)
{
    struct ciuki_recovery_status s;
    if (d->safe_mode || d->terminal_shell) return 0;
    return ciuki_recovery_status(&s) || !((s.boot_flags >> CIUKI_BOOT_SKIP_SHIFT) & CIUKI_RECOVERY_STARTUP);
}
