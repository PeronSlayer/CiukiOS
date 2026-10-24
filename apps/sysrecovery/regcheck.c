/* SPDX-License-Identifier: MIT
 * G14 Registry Checker (docs/design/startup-recovery.md 2.5; Windows 98
 * Resource Kit "Using Registry Checker", Tables 27.8-27.10): RB00n.CRB
 * backups of the registry roots and configuration files, five kept, one a
 * day after a completed start, automatic restore of a damaged root, and
 * ScanReg /Restore. CRB1 is uncompressed (Windows 98 uses CAB):
 *   header 64: "CRB1", u32 version 1, i64 created, u32 count, u32 flags
 *              (1 = started), char comment[40]
 *   count x { char path[96], u32 size, u32 crc32 }, then the data,
 *   then u32 crc32 of everything before it. */
#define _POSIX_C_SOURCE 200809L
/* System headers first: abi.h then leaves the C library's macros alone. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "recovery.h"
#include <ciuki/registry.h>

#define HEADER 64u
#define ENTRY 104u
#define PATH 96u

static const char *const roots[RC_ROOTS] = { "System", "Desktop", "Programs", "Users" };
const char *rc_root_name(unsigned i) { return i < RC_ROOTS ? roots[i] : NULL; }

static uint32_t crc32(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (unsigned k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
    }
    return ~crc;
}
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int full(char *out, size_t cap, const char *root, const char *path)
{
    int n = snprintf(out, cap, "%s%s", root ? root : "", path);
    return n < 0 || (size_t)n >= cap ? -ENAMETOOLONG : 0;
}
static int read_all(const char *path, uint8_t **out, size_t *size, size_t limit)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -errno;
    uint8_t *b = malloc(limit + 1);
    if (!b) { fclose(f); return -ENOMEM; }
    size_t n = fread(b, 1, limit + 1, f);
    int e = ferror(f) ? -EIO : n > limit ? -E2BIG : 0;
    fclose(f);
    if (e) { free(b); return e; }
    *out = b; *size = n; return 0;
}
/* Durable write of a new file (no replacement). */
static int write_new(const char *path, const void *data, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -errno;
    int e = 0;
    const uint8_t *p = data;
    for (size_t done = 0; done < n && !e;) {
        ssize_t w = write(fd, p + done, n - done);
        if (w <= 0) e = w < 0 ? -errno : -EIO; else done += (size_t)w;
    }
    if (!e && fsync(fd)) e = -errno;
    if (close(fd) && !e) e = -errno;
    return e;
}
/* Replace `path` with data: .rcn written, the old file moved to .rco, the
 * new one renamed in (CiukiOS rename refuses an existing target). */
static int replace(const char *path, const void *data, size_t n)
{
    char temp[512], old[512];
    if (snprintf(temp, sizeof(temp), "%s.rcn", path) >= (int)sizeof(temp) ||
        snprintf(old, sizeof(old), "%s.rco", path) >= (int)sizeof(old)) return -ENAMETOOLONG;
    int e = write_new(temp, data, n);
    if (e) { unlink(temp); return e; }
    unlink(old);
    if (rename(path, old) && errno != ENOENT) { e = -errno; unlink(temp); return e; }
    if (rename(temp, path)) { e = -errno; rename(old, path); return e; }
    unlink(old);
    return 0;
}
static int mkdirs(const char *root, const char *path)
{
    char p[512];
    int e = full(p, sizeof(p), root, path); if (e) return e;
    size_t start = strlen(root ? root : "") + 1;
    for (size_t i = start; ; i++) {
        if (p[i] == '/' || !p[i]) {
            char c = p[i]; p[i] = 0;
            if (mkdir(p, 0755) && errno != EEXIST) return -errno;
            p[i] = c;
            if (!c) break;
        }
    }
    return 0;
}

/* The files one backup holds: the four roots, BOOT.CFG, settings *.cfg. */
static int collect(const char *root, char (*paths)[PATH], unsigned *count)
{
    *count = 0;
    char p[512];
    struct stat st;
    for (unsigned i = 0; i < RC_ROOTS; i++) {
        snprintf(paths[*count], PATH, REG_DIRECTORY "/%s.reg", roots[i]);
        if (!full(p, sizeof(p), root, paths[*count]) && !stat(p, &st) && S_ISREG(st.st_mode)) (*count)++;
    }
    snprintf(paths[*count], PATH, "/system/BOOT.CFG");
    if (!full(p, sizeof(p), root, paths[*count]) && !stat(p, &st)) (*count)++;
    if (full(p, sizeof(p), root, "/system/settings")) return -ENAMETOOLONG;
    DIR *d = opendir(p);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && *count < RC_MAX_FILES) {
            size_t n = strlen(e->d_name);
            if (n < 5 || n > 48 || strcasecmp(e->d_name + n - 4, ".cfg")) continue;
            snprintf(paths[*count], PATH, "/system/settings/%s", e->d_name);
            (*count)++;
        }
        closedir(d);
    }
    return 0;
}

/* Parse and verify an archive in memory; entries point into it. */
struct archive { uint8_t *data; size_t size; uint32_t count; const uint8_t *entries, *payload; };
static int archive_open(const char *path, struct archive *a, struct rc_backup *info)
{
    memset(a, 0, sizeof(*a));
    int e = read_all(path, &a->data, &a->size, RC_MAX_BYTES + HEADER + RC_MAX_FILES * ENTRY + 4);
    if (e) return e;
    const uint8_t *b = a->data;
    if (a->size < HEADER + 4 || memcmp(b, "CRB1", 4) || rd32(b + 4) != 1) goto bad;
    a->count = rd32(b + 16);
    if (a->count > RC_MAX_FILES || a->size < HEADER + (size_t)a->count * ENTRY + 4) goto bad;
    if (crc32(0, b, a->size - 4) != rd32(b + a->size - 4)) goto bad;
    a->entries = b + HEADER; a->payload = a->entries + a->count * ENTRY;
    size_t total = 0;
    for (uint32_t i = 0; i < a->count; i++) {
        const uint8_t *en = a->entries + i * ENTRY;
        if (memchr(en, 0, PATH) == NULL || en[0] != '/') goto bad;
        total += rd32(en + PATH);
        if (total > a->size) goto bad;
    }
    if ((size_t)(a->payload - b) + total + 4 != a->size) goto bad;
    size_t off = 0;
    for (uint32_t i = 0; i < a->count; i++) {
        const uint8_t *en = a->entries + i * ENTRY;
        uint32_t n = rd32(en + PATH);
        if (crc32(0, a->payload + off, n) != rd32(en + PATH + 4)) goto bad;
        off += n;
    }
    if (info) {
        int64_t created;
        memcpy(&created, b + 8, 8);
        info->created = created; info->files = a->count; info->bytes = (uint32_t)total;
        info->started = rd32(b + 20) & 1; info->valid = true;
        memcpy(info->comment, b + 24, RC_COMMENT); info->comment[RC_COMMENT - 1] = 0;
    }
    return 0;
bad:
    free(a->data); a->data = NULL;
    return -EILSEQ;
}

int rc_list(const char *root, struct rc_backup *out, unsigned max)
{
    unsigned n = 0;
    char dir[512], path[600];
    int e = full(dir, sizeof(dir), root, RC_BACKUP_DIR); if (e) return e;
    for (unsigned i = 0; i < 100 && n < max; i++) {
        struct rc_backup b; memset(&b, 0, sizeof(b));
        snprintf(b.name, sizeof(b.name), "RB%03u.CRB", i);
        snprintf(path, sizeof(path), "%s/%s", dir, b.name);
        struct stat st;
        if (stat(path, &st)) continue;
        struct archive a;
        if (archive_open(path, &a, &b)) { b.valid = false; b.created = (int64_t)st.st_mtime; }
        else free(a.data);
        out[n++] = b;
    }
    for (unsigned i = 1; i < n; i++)
        for (unsigned j = i; j && out[j - 1].created < out[j].created; j--) {
            struct rc_backup t = out[j]; out[j] = out[j - 1]; out[j - 1] = t;
        }
    return (int)n;
}

int rc_backup(const char *root, const char *comment, int64_t now, bool started, struct rc_backup *made)
{
    char (*paths)[PATH] = calloc(RC_MAX_FILES, PATH);
    struct rc_backup *list = calloc(100, sizeof(*list));
    uint8_t *blob = NULL;
    unsigned count = 0;
    int e = !paths || !list ? -ENOMEM : collect(root, paths, &count);
    if (!e) e = mkdirs(root, RC_BACKUP_DIR);
    int listed = e ? 0 : rc_list(root, list, 100);
    if (listed < 0) e = listed;
    /* Keep at most RC_MAX_BACKUPS - 1 before adding: delete the oldest. */
    char dir[512], path[600];
    if (!e) e = full(dir, sizeof(dir), root, RC_BACKUP_DIR);
    for (int i = listed - 1; !e && i >= RC_MAX_BACKUPS - 1; i--) {
        snprintf(path, sizeof(path), "%s/%s", dir, list[i].name);
        if (unlink(path) && errno != ENOENT) e = -errno;
        list[i].name[0] = 0;
    }
    /* The lowest free RB number. */
    unsigned number = 0;
    for (; !e && number < 100; number++) {
        bool used = false;
        for (int i = 0; i < listed; i++) if (list[i].name[0] && (unsigned)atoi(list[i].name + 2) == number) used = true;
        if (!used) break;
    }
    if (!e && number >= 100) e = -ENOSPC;
    size_t size = HEADER + (size_t)count * ENTRY + 4, used = 0;
    uint8_t **data = calloc(count ? count : 1, sizeof(*data));
    size_t *sizes = calloc(count ? count : 1, sizeof(*sizes));
    if (!e && (!data || !sizes)) e = -ENOMEM;
    for (unsigned i = 0; !e && i < count; i++) {
        char p[512];
        if ((e = full(p, sizeof(p), root, paths[i]))) break;
        if ((e = read_all(p, &data[i], &sizes[i], RC_MAX_BYTES))) break;
        used += sizes[i];
        if (used > RC_MAX_BYTES) e = -E2BIG;
    }
    if (!e && !(blob = malloc(size + used))) e = -ENOMEM;
    if (!e) {
        memset(blob, 0, HEADER + (size_t)count * ENTRY);
        memcpy(blob, "CRB1", 4); wr32(blob + 4, 1); memcpy(blob + 8, &now, 8);
        wr32(blob + 16, count); wr32(blob + 20, started ? 1 : 0);
        if (comment) strncpy((char *)blob + 24, comment, RC_COMMENT - 1);
        uint8_t *payload = blob + HEADER + count * ENTRY;
        for (unsigned i = 0; i < count; i++) {
            uint8_t *en = blob + HEADER + i * ENTRY;
            memcpy(en, paths[i], strlen(paths[i]) + 1);
            wr32(en + PATH, (uint32_t)sizes[i]); wr32(en + PATH + 4, crc32(0, data[i], sizes[i]));
            memcpy(payload, data[i], sizes[i]); payload += sizes[i];
        }
        size += used;
        wr32(blob + size - 4, crc32(0, blob, size - 4));
        snprintf(path, sizeof(path), "%s/RB%03u.CRB", dir, number);
        e = write_new(path, blob, size);
        if (e) unlink(path);
    }
    if (!e && made) {
        memset(made, 0, sizeof(*made));
        snprintf(made->name, sizeof(made->name), "RB%03u.CRB", number);
        made->created = now; made->files = count; made->bytes = (uint32_t)used; made->started = started; made->valid = true;
        if (comment) strncpy(made->comment, comment, RC_COMMENT - 1);
    }
    for (unsigned i = 0; data && i < count; i++) free(data[i]);
    free(data); free(sizes); free(blob); free(list); free(paths);
    return e;
}

static int64_t local_day(int64_t t, int offset_minutes) { int64_t l = t + offset_minutes * 60; return l >= 0 ? l / 86400 : (l - 86399) / 86400; }
int rc_daily(const char *root, int64_t now, int utc_offset_minutes, int *made)
{
    *made = 0;
    struct rc_backup list[RC_MAX_BACKUPS + 4];
    int n = rc_list(root, list, RC_MAX_BACKUPS + 4);
    if (n < 0) return n;
    for (int i = 0; i < n; i++)
        if (list[i].valid && list[i].started &&
            local_day(list[i].created, utc_offset_minutes) == local_day(now, utc_offset_minutes)) return 0;
    int e = rc_backup(root, "Daily backup", now, true, NULL);
    if (!e) *made = 1;
    return e;
}

static int root_valid_dir(const char *dir, unsigned i)
{
    errno = 0;
    struct reg_root *r = reg_open_root_at(roots[i], dir);
    if (r) { reg_close_root(r); return 1; }
    return errno == EINVAL || errno == EIO ? 0 : -errno;
}
int rc_scan(const char *root, unsigned *damaged)
{
    *damaged = 0;
    char dir[512];
    int e = full(dir, sizeof(dir), root, REG_DIRECTORY); if (e) return e;
    for (unsigned i = 0; i < RC_ROOTS; i++) {
        int v = root_valid_dir(dir, i);
        if (v < 0) return v;
        if (!v) *damaged |= 1u << i;
    }
    return 0;
}

/* libreg's FNV-1a text checksum (sdk/libreg/storage.c). */
static uint32_t fnv(const uint8_t *s, size_t n)
{
    uint32_t v = 2166136261u;
    for (size_t i = 0; i < n; i++) v = (v ^ s[i]) * 16777619u;
    return v;
}
static uint64_t current_generation(const char *dir, unsigned i)
{
    struct reg_root *r = reg_open_root_at(roots[i], dir);
    if (!r) return 0;
    uint64_t g = reg_generation(r);
    reg_close_root(r);
    return g == UINT64_MAX ? 0 : g;
}
/* The backup's text with its generation comment replaced by `generation`. */
static char *with_generation(const uint8_t *text, size_t n, uint64_t generation, size_t *out)
{
    char *b = malloc(n + 64);
    if (!b) return NULL;
    size_t used = (size_t)snprintf(b, 64, "; ciuki-registry generation=%llu\n", (unsigned long long)generation);
    static const char prefix[] = "; ciuki-registry generation=";
    for (size_t i = 0; i < n;) {
        size_t end = i;
        while (end < n && text[end] != '\n') end++;
        if (end < n) end++;
        if (!(end - i >= sizeof(prefix) - 1 && !memcmp(text + i, prefix, sizeof(prefix) - 1))) {
            memcpy(b + used, text + i, end - i); used += end - i;
        }
        i = end;
    }
    *out = used;
    return b;
}
/* Install a root text as libreg does: .new, durable intent .txn, old to
 * .bak, .new in, then the artifacts removed. reg_recover finishes it after
 * a power cut at any step. */
static int install_root(const char *dir, unsigned i, const uint8_t *text, size_t n)
{
    char path[600], temp[620], backup[620], journal[620], intent[128];
    snprintf(path, sizeof(path), "%s/%s.reg", dir, roots[i]);
    snprintf(temp, sizeof(temp), "%s.new", path); snprintf(backup, sizeof(backup), "%s.bak", path);
    snprintf(journal, sizeof(journal), "%s.txn", path);
    uint64_t generation = current_generation(dir, i) + 1;
    size_t length; char *body = with_generation(text, n, generation, &length);
    if (!body) return -ENOMEM;
    unlink(temp); unlink(backup); unlink(journal);
    int e = write_new(temp, body, length);
    int count = snprintf(intent, sizeof(intent), "ciuki-registry %llu %llu %08x\n", (unsigned long long)generation,
                         (unsigned long long)length, fnv((const uint8_t *)body, length));
    free(body);
    if (!e) e = write_new(journal, intent, (size_t)count);
    if (!e && rename(path, backup) && errno != ENOENT) e = -errno;
    if (!e && rename(temp, path)) e = -errno;
    if (!e) { unlink(journal); unlink(backup); }
    return e;
}
static int restore_entry(const char *root, const char *dir, const uint8_t *en, const uint8_t *data)
{
    const char *path = (const char *)en;
    uint32_t n = rd32(en + PATH);
    /* Only what rc_backup collects: never another path from an archive. */
    if (strncasecmp(path, REG_DIRECTORY "/", sizeof(REG_DIRECTORY)) && strcasecmp(path, "/system/BOOT.CFG") &&
        strncasecmp(path, "/system/settings/", 17)) return -EINVAL;
    if (strstr(path, "..")) return -EINVAL;
    for (unsigned i = 0; i < RC_ROOTS; i++) {
        char name[PATH]; snprintf(name, sizeof(name), REG_DIRECTORY "/%s.reg", roots[i]);
        if (!strcasecmp(path, name)) return install_root(dir, i, data, n);
    }
    char p[512];
    int e = full(p, sizeof(p), root, path); if (e) return e;
    return replace(p, data, n);
}
int rc_restore(const char *root, const char *name)
{
    char dir[512], path[600], regdir[512];
    int e = full(dir, sizeof(dir), root, RC_BACKUP_DIR); if (e) return e;
    if ((e = full(regdir, sizeof(regdir), root, REG_DIRECTORY))) return e;
    if (strchr(name, '/') || strlen(name) > 12) return -EINVAL;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    struct archive a;
    if ((e = archive_open(path, &a, NULL))) return e;
    if ((e = mkdirs(root, REG_DIRECTORY))) { free(a.data); return e; }
    size_t off = 0;
    for (uint32_t i = 0; !e && i < a.count; i++) {
        const uint8_t *en = a.entries + i * ENTRY;
        e = restore_entry(root, regdir, en, a.payload + off);
        off += rd32(en + PATH);
    }
    free(a.data);
    return e;
}

/* A root's copy in an archive parses: written to a scratch directory and
 * opened with libreg, like the live file. */
static int copy_valid(const char *root, unsigned i, const uint8_t *data, size_t n)
{
    char dir[512], path[600];
    int e = full(dir, sizeof(dir), root, RC_BACKUP_DIR "/CHECK"); if (e) return e;
    mkdir(dir, 0755);
    snprintf(path, sizeof(path), "%s/%s.reg", dir, roots[i]);
    if ((e = write_new(path, data, n))) return e;
    int v = root_valid_dir(dir, i);
    unlink(path); rmdir(dir);
    return v;
}
int rc_autorestore(const char *root, unsigned *restored, char from[16])
{
    *restored = 0; from[0] = 0;
    unsigned damaged;
    int e = rc_scan(root, &damaged);
    if (e || !damaged) return e;
    struct rc_backup list[RC_MAX_BACKUPS + 4];
    int n = rc_list(root, list, RC_MAX_BACKUPS + 4);
    if (n < 0) return n;
    char dir[512], path[600], regdir[512];
    if ((e = full(dir, sizeof(dir), root, RC_BACKUP_DIR)) || (e = full(regdir, sizeof(regdir), root, REG_DIRECTORY))) return e;
    for (unsigned r = 0; r < RC_ROOTS; r++) {
        if (!(damaged & (1u << r))) continue;
        char name[PATH]; snprintf(name, sizeof(name), REG_DIRECTORY "/%s.reg", roots[r]);
        for (int b = 0; b < n && !(*restored & (1u << r)); b++) {
            if (!list[b].valid) continue;
            snprintf(path, sizeof(path), "%s/%s", dir, list[b].name);
            struct archive a;
            if (archive_open(path, &a, NULL)) continue;
            size_t off = 0;
            for (uint32_t i = 0; i < a.count; i++) {
                const uint8_t *en = a.entries + i * ENTRY;
                uint32_t size = rd32(en + PATH);
                if (!strcasecmp((const char *)en, name) && copy_valid(root, r, a.payload + off, size) == 1) {
                    /* The damaged file is set aside, then the good copy installed. */
                    char live[600], kept[620];
                    snprintf(live, sizeof(live), "%s/%s.reg", regdir, roots[r]);
                    snprintf(kept, sizeof(kept), "%s.bad", live);
                    unlink(kept);
                    if (rename(live, kept)) unlink(live);
                    e = install_root(regdir, r, a.payload + off, size);
                    if (!e) { *restored |= 1u << r; if (!from[0]) memcpy(from, list[b].name, 16); }
                    break;
                }
                off += size;
            }
            free(a.data);
            if (e) return e;
        }
    }
    return 0;
}
