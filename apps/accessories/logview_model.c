/* SPDX-License-Identifier: MIT
 * CiukiOS Log Viewer model: which logs exist and their text; see logview.h.
 * sec-sysvc (Windows XP Event Viewer model): the system logs (C:\SYSTEM\LOGS)
 * are SYSTEM's files and administrators read them through the SYSTEM log
 * service; NetSurf's log and the crash reports of the user's programs are the
 * user's own logs (Local Settings\Application Data\CiukiOS\Logs), reached
 * through the same service (ciuki/recovery.h). The Setup log and the crash
 * files earlier releases wrote to C:\WINDOWS\CRASH stay ordinary files. */
#include "logview.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(__CIUKIOS__) && !defined(__linux__)
#include <ciuki/recovery.h>
#endif

const char *lv_root = "";
/* The root of the last scan: entries are read where they were found. */
static const char *scanned_root = "";

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int same_prefix(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++) if (lower((unsigned char)*s) != lower((unsigned char)*prefix)) return 0;
    return 1;
}
static int same(const char *a, const char *b) { return strlen(a) == strlen(b) && same_prefix(a, b); }
/* <prefix><digit>.TXT in any letter case (FAT short names). */
static int numbered(const char *name, const char *prefix)
{
    size_t n = strlen(name), p = strlen(prefix);
    return n == p + 5 && same_prefix(name, prefix) && name[p] >= '0' && name[p] <= '9' && same_prefix(name + p + 1, ".txt");
}

/* ---- log sources ---- */
#if defined(__CIUKIOS__) && !defined(__linux__)
static int log_list(const char *root, unsigned scope, unsigned index, char name[LV_NAME], unsigned long *total)
{
    (void)root; uint32_t bytes = 0;
    if (ciuki_system_log_list(scope, index, name, &bytes)) return -errno;
    *total = bytes; return 0;
}
static int log_read(const char *root, unsigned scope, const char *name, unsigned long offset, void *out, size_t capacity,
                    size_t *got, unsigned long *total)
{
    (void)root; uint32_t bytes = 0;
    if (capacity > CIUKI_SYSTEM_LOG_DATA_MAX) capacity = CIUKI_SYSTEM_LOG_DATA_MAX;
    if (ciuki_system_log_read(scope, name, (uint32_t)offset, out, capacity, got, &bytes)) return -errno;
    *total = bytes; return 0;
}
#else
/* Host fixtures: MACHINE is <root>/system/logs (an administrator's view),
 * USER is <root>/user/logs. Same names and listing rules as the service. */
static void folder(char out[512], const char *root, unsigned scope)
{ snprintf(out, 512, "%s%s", root, scope == LV_MACHINE ? "/system/logs" : "/user/logs"); }
static int log_list(const char *root, unsigned scope, unsigned index, char name[LV_NAME], unsigned long *total)
{
    char dir[512]; folder(dir, root, scope);
    DIR *dp = opendir(dir);
    if (!dp) return -ENOENT;
    struct dirent *e; unsigned seen = 0; int result = -ENOENT;
    while ((e = readdir(dp))) {
        char path[1024]; struct stat st;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (e->d_name[0] == '.' || stat(path, &st) || !S_ISREG(st.st_mode) || strlen(e->d_name) >= LV_NAME) continue;
        if (seen++ == index) { strcpy(name, e->d_name); *total = (unsigned long)st.st_size; result = 0; break; }
    }
    closedir(dp);
    return result;
}
static int log_read(const char *root, unsigned scope, const char *name, unsigned long offset, void *out, size_t capacity,
                    size_t *got, unsigned long *total)
{
    char dir[512], path[1024]; folder(dir, root, scope);
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    *got = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -errno;
    int e = fseek(f, 0, SEEK_END) ? -EIO : 0;
    long size = e ? -1 : ftell(f);
    if (size < 0) e = -EIO;
    else *total = (unsigned long)size;
    if (!e && offset < (unsigned long)size && capacity) {
        if (fseek(f, (long)offset, SEEK_SET)) e = -EIO;
        else { *got = fread(out, 1, capacity, f); if (ferror(f)) e = -EIO; }
    }
    fclose(f);
    return e;
}
#endif

static void add(struct lv_list *l, enum lv_kind kind, unsigned long sequence, const char *title, unsigned scope,
                const char *name, const char *path)
{
    if (l->count >= LV_MAX) return;
    struct lv_entry *e = &l->entry[l->count++];
    e->kind = kind; e->sequence = sequence; e->scope = scope;
    snprintf(e->title, sizeof(e->title), "%s", title);
    snprintf(e->name, sizeof(e->name), "%s", name ? name : "");
    snprintf(e->path, sizeof(e->path), "%s", path);
}
/* A service log's display path; it also identifies the entry on refresh. */
static void display(char out[256], unsigned scope, const char *name)
{
    char shown[LV_NAME]; size_t n = 0;
    for (; name[n] && n < sizeof(shown) - 1; n++) shown[n] = name[n] == '/' ? '\\' : name[n];
    shown[n] = 0;
    snprintf(out, 256, "%s%s", scope == LV_MACHINE ? "C:\\SYSTEM\\LOGS\\" : "%USERPROFILE%\\Local Settings\\Application Data\\CiukiOS\\Logs\\", shown);
}
/* "CiukiOS crash sequence=N" and "Executable: NAME": the first two lines of
 * a crash file (apps/desktop/win32_crash.c); 0 when it is not one. */
static int crash_text(const char *text, size_t n, unsigned long *sequence, char name[48])
{
    char copy[256]; if (n >= sizeof(copy)) n = sizeof(copy) - 1;
    memcpy(copy, text, n); copy[n] = 0;
    if (sscanf(copy, "CiukiOS crash sequence=%lu", sequence) != 1) return 0;
    name[0] = 0;
    const char *line = strchr(copy, '\n');
    if (line && !strncmp(line + 1, "Executable: ", 12)) {
        size_t k = 0;
        for (const char *p = line + 13; *p && *p != '\n' && *p != '\r' && k < 47; p++) name[k++] = *p;
        name[k] = 0;
    }
    return 1;
}
static void add_crash(struct lv_list *l, int win32, unsigned long sequence, const char *exe, unsigned scope,
                      const char *name, const char *path)
{
    char title[64], tail[40];
    snprintf(tail, sizeof(tail), "%s", exe[0] ? exe : "unknown program");
    snprintf(title, sizeof(title), "%s %lu: %s", win32 ? "Win32 crash" : "Crash", sequence, tail);
    add(l, LV_CRASH, sequence, title, scope, name, path);
}
/* Crash reports of a service scope: CRASHn.TXT (native), WCRASHn.TXT (Win32),
 * also below the held logs of a session without a profile (Users/u<uid>/). */
static void scan_crashes(struct lv_list *l, const char *root, unsigned scope)
{
    char name[LV_NAME]; unsigned long total;
    for (unsigned i = 0; l->count < LV_MAX && !log_list(root, scope, i, name, &total); i++) {
        const char *leaf = strrchr(name, '/'); leaf = leaf ? leaf + 1 : name;
        int win32 = numbered(leaf, "WCRASH");
        if (!win32 && !numbered(leaf, "CRASH")) continue;
        char head[256]; size_t got = 0; unsigned long size = 0, sequence = 0; char exe[48];
        if (log_read(root, scope, name, 0, head, sizeof(head), &got, &size) || !crash_text(head, got, &sequence, exe)) continue;
        char path[256]; display(path, scope, name);
        add_crash(l, win32, sequence, exe, scope, name, path);
    }
}
void lv_scan(struct lv_list *l, const char *root)
{
    memset(l, 0, sizeof(*l));
    scanned_root = root ? root : "";
    char path[256];
    display(path, LV_MACHINE, "BOOT.LOG"); add(l, LV_BOOT, 0, "Boot log", LV_MACHINE, "BOOT.LOG", path);
    display(path, LV_USER, "NETSURF.LOG"); add(l, LV_NETSURF, 0, "NetSurf log", LV_USER, "NETSURF.LOG", path);
    char setup[512];
    snprintf(setup, sizeof(setup), "%s/SYSTEM/SETUP.LOG", root);
    FILE *f = fopen(setup, "rb");
    if (f) { fclose(f); add(l, LV_SETUP, 0, "Setup log", 0, NULL, setup); }
    /* The other system logs, when present (administrators). */
    static const struct { const char *name, *title; enum lv_kind kind; } named[] = {
        {"BOOT1.LOG", "Previous boot log", LV_BOOT}, {"BOOT2.LOG", "Older boot log", LV_BOOT},
        {"SCANDISK.LOG", "ScanDisk log", LV_SYSTEM}, {"SFC.LOG", "System File Checker log", LV_SYSTEM},
        {"SELFTEST.LOG", "Self-test log", LV_SYSTEM}, {"NETSURF.LOG", "Earlier NetSurf log", LV_NETSURF} };
    for (unsigned i = 0; i < sizeof(named) / sizeof(*named); i++) {
        char probe[1]; size_t got; unsigned long total;
        if (log_read(root, LV_MACHINE, named[i].name, 0, probe, 0, &got, &total)) continue;
        display(path, LV_MACHINE, named[i].name);
        add(l, named[i].kind, 0, named[i].title, LV_MACHINE, named[i].name, path);
    }
    /* NetSurf logs held for a session without a profile (first start). */
    char name[LV_NAME]; unsigned long total;
    for (unsigned i = 0; l->count < LV_MAX && !log_list(root, LV_MACHINE, i, name, &total); i++) {
        const char *leaf = strrchr(name, '/');
        if (!leaf || !same(leaf + 1, "NETSURF.LOG")) continue;
        display(path, LV_MACHINE, name);
        add(l, LV_NETSURF, 0, "NetSurf log (first start)", LV_MACHINE, name, path);
    }
    unsigned first = l->count;
    scan_crashes(l, root, LV_USER);
    scan_crashes(l, root, LV_MACHINE);
    /* Win32 crash files of earlier releases. */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/WINDOWS/CRASH", root);
    DIR *dp = opendir(dir);
    if (dp) {
        struct dirent *e;
        while ((e = readdir(dp)) && l->count < LV_MAX) {
            if (!numbered(e->d_name, "CRASH")) continue;
            char file[768], head[256]; unsigned long sequence = 0; char exe[48];
            snprintf(file, sizeof(file), "%s/%s", dir, e->d_name);
            FILE *in = fopen(file, "rb");
            if (!in) continue;
            size_t got = fread(head, 1, sizeof(head), in); fclose(in);
            if (!crash_text(head, got, &sequence, exe)) continue;
            add_crash(l, 1, sequence, exe, 0, NULL, file);
        }
        closedir(dp);
    }
    /* Newest first; equal sequences (native and Windows crash files count separately) keep scan order. */
    for (unsigned i = first + 1; i < l->count; i++)
        for (unsigned j = i; j > first && l->entry[j].sequence > l->entry[j - 1].sequence; j--) {
            struct lv_entry t = l->entry[j]; l->entry[j] = l->entry[j - 1]; l->entry[j - 1] = t;
        }
}

/* The read tail: from the next line when cut, NUL as a space, CR LF as LF. */
static size_t normalize(char *out, size_t got, int cut)
{
    size_t start = 0;
    if (cut) {
        while (start < got && out[start] != '\n') start++;
        if (start < got) start++; else start = 0;
    }
    size_t n = 0;
    for (size_t i = start; i < got; i++) {
        char c = out[i];
        if (c == '\r') { out[n++] = '\n'; if (i + 1 < got && out[i + 1] == '\n') i++; }
        else out[n++] = c ? c : ' ';
    }
    return n;
}
int lv_read_tail(const char *path, char *out, size_t capacity, size_t *length, unsigned long *total)
{
    *length = 0; *total = 0;
    if (!capacity) return -EINVAL;
    FILE *f = fopen(path, "rb");
    if (!f) return -errno;
    if (fseek(f, 0, SEEK_END)) { int e = errno; fclose(f); return -(e ? e : EIO); }
    long size = ftell(f);
    if (size < 0) { fclose(f); return -EIO; }
    *total = (unsigned long)size;
    size_t want = (unsigned long)size > capacity ? capacity : (size_t)size;
    if (fseek(f, size - (long)want, SEEK_SET)) { fclose(f); return -EIO; }
    size_t got = fread(out, 1, want, f);
    int failed = ferror(f);
    fclose(f);
    if (failed) return -EIO;
    *length = normalize(out, got, want < (size_t)size);
    return 0;
}
int lv_read_entry(const struct lv_entry *e, char *out, size_t capacity, size_t *length, unsigned long *total)
{
    if (!e->scope) return lv_read_tail(e->path, out, capacity, length, total);
    *length = 0; *total = 0;
    if (!capacity) return -EINVAL;
    size_t got = 0; unsigned long size = 0;
    int r = log_read(scanned_root, e->scope, e->name, 0, out, 0, &got, &size);
    if (r) return r;
    size_t want = size > capacity ? capacity : (size_t)size, done = 0;
    unsigned long from = size - want;
    while (done < want) {
        unsigned long now = 0;
        r = log_read(scanned_root, e->scope, e->name, from + done, out + done, want - done, &got, &now);
        if (r) return r;
        if (!got) break;
        done += got;
    }
    *total = size;
    *length = normalize(out, done, want < size);
    return 0;
}
