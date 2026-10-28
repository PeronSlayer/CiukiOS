/* SPDX-License-Identifier: MIT
 * G14 Terminal commands (docs/design/startup-recovery.md 2.4-2.6; Windows
 * 98 Resource Kit Tables 10.7 and 27.9): SCANREG (/BACKUP, /COMMENT=,
 * /RESTORE [n], /FIX), SCANDISK [d:] [/A] [/THOROUGH] [/AUTOFIX], SFC
 * [/UPDATE path] [/RESTORE path source], and WIN. */
#include "terminal.h"
#include <ciuki/recovery.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "../sysrecovery/recovery.h"

static int is(const char *a, const char *b) { return !strcasecmp(a, b); }
static int command_prompt_only(void)
{
    struct ciuki_recovery_status s;
    return !ciuki_recovery_status(&s) && (s.boot_flags & CIUKI_BOOT_CMD_ONLY);
}
static int started(void)
{
    struct ciuki_recovery_status s;
    return !ciuki_recovery_status(&s) && (s.flags & CIUKI_RECOVERY_COMPLETE);
}
static void when(int64_t t, char *out, size_t cap)
{
    time_t v = (time_t)t; struct tm tm;
    if (localtime_r(&v, &tm)) strftime(out, cap, "%Y-%m-%d %H:%M", &tm); else snprintf(out, cap, "?");
}

static int scanreg(struct terminal *t, int n, char **a)
{
    const char *comment = NULL; int backup = 0, restore = 0, fix = 0, pick = 0;
    for (int i = 1; i < n; i++) {
        if (is(a[i], "/BACKUP")) backup = 1;
        else if (!strncasecmp(a[i], "/COMMENT=", 9)) comment = a[i] + 9;
        else if (is(a[i], "/RESTORE")) restore = 1;
        else if (is(a[i], "/FIX") || is(a[i], "/AUTOSCAN") || is(a[i], "/SCANONLY")) fix = is(a[i], "/FIX");
        else if (restore && !pick && atoi(a[i]) > 0) pick = atoi(a[i]);
        else return -EINVAL;
    }
    struct rc_backup list[RC_MAX_BACKUPS + 4];
    char date[32];
    /* sec-sysvc2: Registry Checker runs in the SYSTEM registry broker and is
     * an administrator tool (Windows 98 ScanReg /Restore, XP System Restore). */
    static const char admin_only[] = "Registry Checker can be used by administrators only.\n";
    if (restore) {
        int count = rc_list("", list, RC_MAX_BACKUPS + 4);
        if (count == -EPERM) { term_write(t, admin_only); return count; }
        if (count < 0) return count;
        if (!count) { term_write(t, "Registry Checker found no backups in C:\\SYSTEM\\SYSBCKUP.\n"); return 0; }
        if (!pick) {
            term_write(t, "Choose a backup to restore: SCANREG /RESTORE n\n");
            for (int i = 0; i < count; i++) {
                when(list[i].created, date, sizeof(date));
                term_printf(t, "  %d. %s  %s  %s%s%s\n", i + 1, list[i].name, date,
                            list[i].valid ? (list[i].started ? "Started" : "Not started") : "Damaged",
                            list[i].comment[0] ? "  " : "", list[i].comment);
            }
            return 0;
        }
        if (pick > count || !list[pick - 1].valid) { term_write(t, "That backup cannot be restored.\n"); return -EINVAL; }
        /* Windows 98 restores an earlier registry outside Windows; sec-sysvc2:
         * the registry broker restores it at the next start, before any
         * program uses the registry (XP's System Restore restarts as well). */
        int e = rc_restore("", list[pick - 1].name);
        if (e) return e;
        when(list[pick - 1].created, date, sizeof(date));
        term_printf(t, "Registry Checker will restore %s (%s) when CiukiOS starts again.\nType REBOOT to restart CiukiOS.\n",
                    list[pick - 1].name, date);
        return 0;
    }
    unsigned damaged = 0;
    int e = rc_scan("", &damaged);
    if (e) return e;
    /* Only administrators repair or back up (EPERM below for the others). */
    if ((fix && damaged) || backup || comment) {
        struct rc_backup probe;
        if (rc_list("", &probe, 1) == -EPERM) { term_write(t, admin_only); return -EPERM; }
    }
    if (damaged) {
        for (unsigned i = 0; i < RC_ROOTS; i++) if (damaged & (1u << i)) term_printf(t, "The %s root of the registry is damaged.\n", rc_root_name(i));
        if (fix) {
            unsigned restored; char from[16];
            if ((e = rc_autorestore("", &restored, from))) return e;
            if (restored) term_printf(t, "Registry Checker restored it from %s. Type REBOOT to restart.\n", from);
            else term_write(t, "No backup holds a good copy.\n");
        } else term_write(t, "Type SCANREG /FIX to restore it from the newest good backup.\n");
    } else term_write(t, "Registry Checker found no errors in the system registry.\n");
    if (backup || comment) {
        struct rc_backup made;
        if ((e = rc_backup("", comment, (int64_t)time(NULL), started(), &made))) return e;
        term_printf(t, "Registry and configuration files backed up to C:\\SYSTEM\\SYSBCKUP\\%s.\n", made.name);
    }
    return 0;
}

static int drive_of(const char *s, unsigned *drive)
{
    if (strlen(s) == 2 && s[1] == ':') {
        char c = s[0] | 0x20;
        if (c >= 'a' && c <= 'z') { *drive = (unsigned)(c - 'a'); return 1; }
    }
    return 0;
}
static int scandisk(struct terminal *t, int n, char **a)
{
    unsigned drives[26], count = 0, drive; uint32_t flags = 0; int all = 0;
    for (int i = 1; i < n; i++) {
        if (drive_of(a[i], &drive)) { if (count < 26) drives[count++] = drive; }
        else if (is(a[i], "/A") || is(a[i], "/ALL")) all = 1;
        else if (is(a[i], "/THOROUGH") || is(a[i], "/SURFACE")) flags |= CIUKI_SCANDISK_THOROUGH;
        else if (is(a[i], "/AUTOFIX")) flags |= CIUKI_SCANDISK_FIX;
        else if (is(a[i], "/P") || is(a[i], "/N")) continue;
        else return -EINVAL;
    }
    if (all) for (count = 0, drive = 2; drive < 26; drive++) drives[count++] = drive;
    if (!count) {
        drives[count++] = !strncmp(t->cwd, "/mnt/", 5) && t->cwd[5] ? (unsigned)((t->cwd[5] | 0x20) - 'a') : 2;
    }
    int checked = 0;
    for (unsigned k = 0; k < count; k++) {
        struct ciuki_scandisk r;
        char text[2048];
        if (ciuki_scandisk(drives[k], flags, &r)) {
            if (all && errno == ENODEV) continue;
            term_printf(t, "ScanDisk cannot check drive %c: (%s).\n", 'A' + drives[k], strerror(errno));
            continue;
        }
        if (t->host.pump) t->host.pump(t->host.ctx);
        term_printf(t, "ScanDisk, drive %c: (%s)\n", 'A' + drives[k], flags & CIUKI_SCANDISK_THOROUGH ? "Thorough" : "Standard");
        scandisk_text(&r, text, sizeof(text));
        term_write(t, text);
        (void)scandisk_log("", &r, (int64_t)time(NULL));
        if (r.flags & CIUKI_SCANDISK_SCHEDULED) term_write(t, "Type REBOOT to restart and fix the errors now.\n");
        else if ((r.flags & CIUKI_SCANDISK_ERRORS) && !(flags & CIUKI_SCANDISK_FIX))
            term_write(t, "Type SCANDISK /AUTOFIX to repair them at the next start.\n");
        checked++;
    }
    return checked || !count ? 0 : -ENODEV;
}

static int sfc(struct terminal *t, int n, char **a)
{
    char path[FM_PATH], source[FM_PATH];
    int e;
    if (n == 3 && is(a[1], "/UPDATE")) {
        if ((e = term_path(t, a[2], path))) return e;
        if ((e = sfc_update("", path))) return e;
        term_printf(t, "The verification information of %s was updated.\n", path);
        return 0;
    }
    if (n == 4 && is(a[1], "/RESTORE")) {
        if ((e = term_path(t, a[2], path)) || (e = term_path(t, a[3], source))) return e;
        if (!strcmp(source, "/")) source[0] = 0;
        e = sfc_restore("", path, source);
        if (e == -EILSEQ) { term_write(t, "The source file is not the original CiukiOS file.\n"); return e; }
        if (e) return e;
        term_printf(t, "%s was restored; the previous file is in C:\\SYSTEM\\HELPDESK\\SFC.\n", path);
        return 0;
    }
    if (n != 1) return -EINVAL;
    struct sfc_result r;
    if ((e = sfc_scan("", &r))) return e;
    for (unsigned i = 0; i < r.count; i++)
        term_printf(t, "%s %s\n", r.problems[i].state == SFC_DELETED ? "Deleted:" : "Changed:", r.problems[i].path);
    term_printf(t, "System File Checker: %u files checked, %u changed, %u deleted.\n", r.checked, r.changed, r.deleted);
    if (r.count) term_write(t, "SFC /UPDATE file keeps a new version; SFC /RESTORE file source restores the original.\n");
    (void)sfc_log("", &r, (int64_t)time(NULL));
    sfc_free(&r);
    return 0;
}

/* 1 when a[0] is one of these commands; *result is its outcome. */
int term_recovery(struct terminal *t, int n, char **a, int *result)
{
    if (is(a[0], "SCANREG")) *result = scanreg(t, n, a);
    else if (is(a[0], "SCANDISK")) *result = scandisk(t, n, a);
    else if (is(a[0], "SFC")) *result = sfc(t, n, a);
    else if (is(a[0], "WIN")) {
        *result = n != 1 ? -EINVAL : !command_prompt_only() ? (term_write(t, "The desktop is already running.\n"), 0) :
            t->host.launch ? t->host.launch(t->host.ctx, "win", NULL) : -ENOSYS;
    } else return 0;
    return 1;
}
