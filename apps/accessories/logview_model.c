/* SPDX-License-Identifier: MIT
 * CiukiOS Log Viewer model: which logs exist and their text; see logview.h. */
#include "logview.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *lv_root = "";
static const char *const crash_dirs[] = { "/system/logs", "/WINDOWS/CRASH" };

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int same_prefix(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++) if (lower((unsigned char)*s) != lower((unsigned char)*prefix)) return 0;
    return 1;
}
/* CRASH<digit>.TXT in any letter case (FAT short names). */
static int crash_name(const char *name)
{
    size_t n = strlen(name);
    return n == 10 && same_prefix(name, "crash") && name[5] >= '0' && name[5] <= '9' && same_prefix(name + 6, ".txt");
}
static void add(struct lv_list *l, enum lv_kind kind, unsigned long sequence, const char *title, const char *root, const char *path)
{
    if (l->count >= LV_MAX) return;
    struct lv_entry *e = &l->entry[l->count++];
    e->kind = kind; e->sequence = sequence;
    snprintf(e->title, sizeof(e->title), "%s", title);
    snprintf(e->path, sizeof(e->path), "%s%s", root, path);
}
/* "CiukiOS crash sequence=N" and "Executable: NAME": the first two lines of
 * a crash file (apps/desktop/win32_crash.c); 0 when the file is not one. */
static int crash_header(const char *path, unsigned long *sequence, char name[48])
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char line[160];
    int ok = fgets(line, sizeof(line), f) && sscanf(line, "CiukiOS crash sequence=%lu", sequence) == 1;
    name[0] = 0;
    if (ok && fgets(line, sizeof(line), f) && !strncmp(line, "Executable: ", 12)) {
        size_t n = 0;
        for (const char *p = line + 12; *p && *p != '\n' && *p != '\r' && n < 47; p++) name[n++] = *p;
        name[n] = 0;
    }
    fclose(f);
    return ok;
}
void lv_scan(struct lv_list *l, const char *root)
{
    memset(l, 0, sizeof(*l));
    add(l, LV_BOOT, 0, "Boot log", root, "/system/logs/BOOT.LOG");
    add(l, LV_NETSURF, 0, "NetSurf log", root, "/system/logs/NETSURF.LOG");
    char setup[256];
    snprintf(setup, sizeof(setup), "%s/SYSTEM/SETUP.LOG", root);
    FILE *f = fopen(setup, "rb");
    if (f) { fclose(f); add(l, LV_SETUP, 0, "Setup log", root, "/SYSTEM/SETUP.LOG"); }
    unsigned first = l->count;
    for (unsigned d = 0; d < sizeof(crash_dirs) / sizeof(crash_dirs[0]); d++) {
        char dir[256];
        snprintf(dir, sizeof(dir), "%s%s", root, crash_dirs[d]);
        DIR *dp = opendir(dir);
        if (!dp) continue;
        struct dirent *e;
        while ((e = readdir(dp)) && l->count < LV_MAX) {
            if (!crash_name(e->d_name)) continue;
            char path[320], name[48];
            unsigned long sequence = 0;
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            if (!crash_header(path, &sequence, name)) continue;
            char title[64], tail[40];
            snprintf(tail, sizeof(tail), "%s", name[0] ? name : "unknown program");
            snprintf(title, sizeof(title), "%s %lu: %s", d ? "Win32 crash" : "Crash", sequence, tail);
            char rel[128];
            snprintf(rel, sizeof(rel), "%s/%s", crash_dirs[d], e->d_name);
            add(l, LV_CRASH, sequence, title, root, rel);
        }
        closedir(dp);
    }
    /* Newest first; equal sequences (native and Windows crash files count separately) keep scan order. */
    for (unsigned i = first + 1; i < l->count; i++)
        for (unsigned j = i; j > first && l->entry[j].sequence > l->entry[j - 1].sequence; j--) {
            struct lv_entry t = l->entry[j]; l->entry[j] = l->entry[j - 1]; l->entry[j - 1] = t;
        }
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
    size_t start = 0;
    if (want < (size_t)size) {                       /* cut: begin at the next line */
        while (start < got && out[start] != '\n') start++;
        if (start < got) start++; else start = 0;
    }
    size_t n = 0;
    for (size_t i = start; i < got; i++) {
        char c = out[i];
        if (c == '\r') { out[n++] = '\n'; if (i + 1 < got && out[i + 1] == '\n') i++; }
        else out[n++] = c ? c : ' ';
    }
    *length = n;
    return 0;
}
