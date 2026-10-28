/* SPDX-License-Identifier: MIT
 * G14 Registry Checker (docs/design/startup-recovery.md 2.5; Windows 98
 * Resource Kit "Using Registry Checker", Tables 27.8-27.10): RB00n.CRB
 * backups of the registry and configuration files, five kept, one a day
 * after a completed start, automatic restore of a damaged root, and
 * ScanReg /Restore. CRB1 is uncompressed (Windows 98 uses CAB):
 *   header 64: "CRB1", u32 version 1, i64 created, u32 count, u32 flags
 *              (1 = started), char comment[40]
 *   count x { char path[96], u32 size, u32 crc32 }, then the data,
 *   then u32 crc32 of everything before it.
 *
 * sec-sysvc2 (docs/validation/sec-sysvc2.md): the registry is the SYSTEM
 * registry broker's since sec-k06, so Registry Checker works there. The
 * functions below are the engine the broker runs as SYSTEM (apps/registry/
 * checker.c, built with RC_LOCAL) and the host fixtures use (root not "").
 * On CiukiOS every other program's call with root "" is a request to the
 * broker: listing, backups, restores and repairs are administrator-only, as
 * the Windows 98 Registry Checker restore and Windows XP System Restore are;
 * the daily backup is the broker's SYSTEM job for every session; a restore
 * is applied by the broker at the next start, before any program uses the
 * registry (XP's System Restore also restores while the computer restarts).
 * A backup holds the machine registry (the broker's native System and
 * Programs roots and the Win32 machine hive slots), BOOT.CFG, HWPROF.CFG
 * and the .cfg files of C:\SYSTEM\SETTINGS. */
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
#include "../../sdk/libreg/internal.h"
#include "../../sdk/private/registry_protocol.h"
/* RC_REMOTE_TEST: the host broker test (tests/host/security/
 * registry_checker_test.c) drives this client against the production broker. */
#if (defined(__CIUKIOS__) && !defined(__linux__) && !defined(RC_LOCAL)) || defined(RC_REMOTE_TEST)
#define RC_REMOTE 1
#include "../../sdk/libreg/transport.h"
#else
#define RC_REMOTE 0
#endif

#define HEADER 64u
#define ENTRY 104u
#define PATH 96u
#define PENDING RC_BACKUP_DIR "/RESTORE.PND"

static const char *const roots[RC_ROOTS] = { "System", "Programs" };
const char *rc_root_name(unsigned i) { return i < RC_ROOTS ? roots[i] : i == RC_ROOTS ? "Win32 machine" : NULL; }

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

/* ---- the machine registry (the broker's files, sec-k06) ---- */
/* The root's committed text (a cut write is finished first, as the broker's
 * own reader does). -ENOENT: no such root. */
static int root_text(const char *root, unsigned i, char **text, size_t *n)
{
    char path[512];
    int e = full(path, sizeof(path), root, RC_MACHINE "/");
    if (!e && strlen(path) + strlen(roots[i]) + 5 > sizeof(path)) e = -ENAMETOOLONG;
    if (e) return e;
    strcat(path, roots[i]); strcat(path, ".reg");
    if (reg_recover(path, roots[i]) && errno != ENOENT) return -(errno ? errno : EIO);
    if (reg_file_read(path, text, n)) return -(errno ? errno : EIO);
    return 0;
}
/* 1 valid, 0 damaged, -errno. */
static int root_valid(const char *name, const char *text, size_t n)
{
    struct rt *t = reg_parse(name, text, n);
    if (!t) return 0;
    reg_tree_free(t);
    return 1;
}
static uint64_t current_generation(const char *root, unsigned i)
{
    char *text = NULL; size_t n = 0;
    if (root_text(root, i, &text, &n)) return 0;
    struct rt *t = reg_parse(roots[i], text, n);
    free(text);
    uint64_t g = t ? t->generation : 0;
    reg_tree_free(t);
    return g == UINT64_MAX ? 0 : g;
}
static const char *const slots[RC_SLOTS] = { RC_MACHINE "/SYSTEM.CRG.0", RC_MACHINE "/SYSTEM.CRG.1" };
/* A committed Win32 machine hive slot (apps/w32srv/storage.c: 40-byte CRG1
 * header for hive 0, CRC-32 of the first 28 header bytes and of the text,
 * the COMMITTED mark); its loader takes the valid slot with the higher
 * sequence, so only that one is backed up. */
#define CRG_HEADER 40u
static bool slot_valid(const uint8_t *b, size_t n, uint64_t *sequence)
{
    if (n < CRG_HEADER || memcmp(b, "CRG1", 4) || rd32(b + 4) != 1 || rd32(b + 8) != 0 || rd32(b + 20) > RC_MAX_BYTES ||
        n != CRG_HEADER + (size_t)rd32(b + 20) || rd32(b + 36) || rd32(b + 28) != crc32(0, b, 28) ||
        rd32(b + 24) != crc32(0, b + CRG_HEADER, n - CRG_HEADER) || rd32(b + 32) != 0x43524731u) return false;
    *sequence = rd32(b + 12) | (uint64_t)rd32(b + 16) << 32;
    return *sequence && *sequence != UINT64_MAX;
}
static int active_slot(const char *root)
{
    int best = -1; uint64_t top = 0;
    for (unsigned i = 0; i < RC_SLOTS; i++) {
        char p[512]; uint8_t *b = NULL; size_t n = 0; uint64_t sequence = 0;
        if (full(p, sizeof(p), root, slots[i]) || read_all(p, &b, &n, RC_MAX_BYTES + CRG_HEADER)) continue;
        if (slot_valid(b, n, &sequence) && sequence > top) { top = sequence; best = (int)i; }
        free(b);
    }
    return best;
}

/* The files one backup holds. */
static int collect(const char *root, char (*paths)[PATH], unsigned *count)
{
    *count = 0;
    char p[512];
    struct stat st;
    for (unsigned i = 0; i < RC_ROOTS; i++) {
        char *text = NULL; size_t n;
        int e = root_text(root, i, &text, &n);
        free(text);
        if (e == -ENOENT) continue;
        if (e) return e;
        snprintf(paths[(*count)++], PATH, RC_MACHINE "/%s.reg", roots[i]);
    }
    int slot = active_slot(root);
    if (slot >= 0) snprintf(paths[(*count)++], PATH, "%s", slots[slot]);
    static const char *const files[] = { "/system/BOOT.CFG", "/system/HWPROF.CFG" };
    for (unsigned i = 0; i < 2; i++)
        if (!full(p, sizeof(p), root, files[i]) && !stat(p, &st) && S_ISREG(st.st_mode)) snprintf(paths[(*count)++], PATH, "%s", files[i]);
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
static bool backup_name(const char *name)
{
    return name && strlen(name) == 9 && !strncmp(name, "RB", 2) && name[2] >= '0' && name[2] <= '9' &&
           name[3] >= '0' && name[3] <= '9' && name[4] >= '0' && name[4] <= '9' && !strcmp(name + 5, ".CRB");
}

static int local_list(const char *root, struct rc_backup *out, unsigned max)
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

static int local_backup(const char *root, const char *comment, int64_t now, bool started, struct rc_backup *made)
{
    char (*paths)[PATH] = calloc(RC_MAX_FILES, PATH);
    struct rc_backup *list = calloc(100, sizeof(*list));
    uint8_t *blob = NULL;
    unsigned count = 0;
    int e = !paths || !list ? -ENOMEM : collect(root, paths, &count);
    if (!e) e = mkdirs(root, RC_BACKUP_DIR);
    int listed = e ? 0 : local_list(root, list, 100);
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
static int local_daily(const char *root, int64_t now, int utc_offset_minutes, int *made)
{
    *made = 0;
    struct rc_backup list[RC_MAX_BACKUPS + 4];
    int n = local_list(root, list, RC_MAX_BACKUPS + 4);
    if (n < 0) return n;
    for (int i = 0; i < n; i++)
        if (list[i].valid && list[i].started &&
            local_day(list[i].created, utc_offset_minutes) == local_day(now, utc_offset_minutes)) return 0;
    int e = local_backup(root, "Daily backup", now, true, NULL);
    if (!e) *made = 1;
    return e;
}

static int local_scan(const char *root, unsigned *damaged)
{
    *damaged = 0;
    for (unsigned i = 0; i < RC_ROOTS; i++) {
        char *text = NULL; size_t n = 0;
        int e = root_text(root, i, &text, &n);
        if (e == -ENOENT) continue;
        if (e == -EIO || e == -EINVAL) { *damaged |= 1u << i; continue; }   /* recovery itself failed */
        if (e) return e;
        if (!root_valid(roots[i], text, n)) *damaged |= 1u << i;
        free(text);
    }
    return 0;
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
/* Install a root text with libreg's journal protocol (.new, durable intent
 * .txn, old to .bak, .new in), at a generation above the current one, so
 * readers notice the change; reg_recover finishes it after a power cut. */
static int install_root(const char *root, unsigned i, const uint8_t *text, size_t n)
{
    char path[512];
    int e = full(path, sizeof(path), root, RC_MACHINE "/");
    if (!e && strlen(path) + strlen(roots[i]) + 5 > sizeof(path)) e = -ENAMETOOLONG;
    if (e) return e;
    strcat(path, roots[i]); strcat(path, ".reg");
    uint64_t generation = current_generation(root, i) + 1;
    size_t length; char *body = with_generation(text, n, generation, &length);
    if (!body) return -ENOMEM;
    e = reg_file_write(path, roots[i], body, length, generation) ? -(errno ? errno : EIO) : 0;
    free(body);
    return e;
}
/* Only what collect() gathers, and the legacy roots of pre-sec-k06 backups. */
static bool entry_known(const char *path)
{
    if (strstr(path, "..")) return false;
    if (!strncasecmp(path, RC_MACHINE "/", sizeof(RC_MACHINE))) {
        const char *leaf = path + sizeof(RC_MACHINE);
        for (unsigned i = 0; i < RC_ROOTS; i++) {
            char name[PATH]; snprintf(name, sizeof(name), "%s.reg", roots[i]);
            if (!strcasecmp(leaf, name)) return true;
        }
        for (unsigned i = 0; i < RC_SLOTS; i++) if (!strcasecmp(path, slots[i])) return true;
        return false;
    }
    if (!strncasecmp(path, REG_DIRECTORY "/", sizeof(REG_DIRECTORY))) return !strchr(path + sizeof(REG_DIRECTORY), '/');
    if (!strcasecmp(path, "/system/BOOT.CFG") || !strcasecmp(path, "/system/HWPROF.CFG")) return true;
    return !strncasecmp(path, "/system/settings/", 17) && path[17] && !strchr(path + 17, '/');
}
/* Legacy entries of pre-sec-k06 backups (the /System/Registry roots nothing
 * reads any more) are skipped. *slot gets the machine hive slots found. */
static int restore_entry(const char *root, const uint8_t *en, const uint8_t *data, unsigned *slot_seen)
{
    const char *path = (const char *)en;
    uint32_t n = rd32(en + PATH);
    if (strstr(path, "..")) return -EINVAL;
    for (unsigned i = 0; i < RC_ROOTS; i++) {
        char name[PATH]; snprintf(name, sizeof(name), RC_MACHINE "/%s.reg", roots[i]);
        if (!strcasecmp(path, name)) return install_root(root, i, data, n);
    }
    if (!strncasecmp(path, REG_DIRECTORY "/", sizeof(REG_DIRECTORY))) return 0;
    char p[512];
    int e = full(p, sizeof(p), root, path); if (e) return e;
    for (unsigned i = 0; i < RC_SLOTS; i++)
        if (!strcasecmp(path, slots[i])) { *slot_seen |= 1u << i; return replace(p, data, n); }
    if (!strcasecmp(path, "/system/BOOT.CFG")) {
        /* sec-sysvc: BOOT.CFG is sealed; the SYSTEM service restores it. */
        if (n <= CIUKI_BOOT_CONFIG_MAX && !sr_boot_config(root, (const char *)data, n, &e)) return e;
        return replace(p, data, n);
    }
    if (!strcasecmp(path, "/system/HWPROF.CFG")) return replace(p, data, n);
    if (!strncasecmp(path, "/system/settings/", 17) && !strchr(path + 17, '/')) return replace(p, data, n);
    return -EINVAL;
}
static int local_restore(const char *root, const char *name)
{
    char dir[512], path[600];
    int e = full(dir, sizeof(dir), root, RC_BACKUP_DIR); if (e) return e;
    if (!backup_name(name)) return -EINVAL;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    struct archive a;
    if ((e = archive_open(path, &a, NULL))) return e;
    /* Every entry must be one a backup holds, and every registry root and
     * hive slot must be valid, before anything changes. */
    size_t off = 0; unsigned seen = 0;
    for (uint32_t i = 0; !e && i < a.count; i++) {
        const uint8_t *en = a.entries + i * ENTRY;
        if (!entry_known((const char *)en)) e = -EINVAL;
        uint64_t sequence;
        for (unsigned k = 0; k < RC_SLOTS; k++)
            if (!e && !strcasecmp((const char *)en, slots[k]) && !slot_valid(a.payload + off, rd32(en + PATH), &sequence)) e = -EILSEQ;
        for (unsigned r = 0; r < RC_ROOTS; r++) {
            char name[PATH]; snprintf(name, sizeof(name), RC_MACHINE "/%s.reg", roots[r]);
            if (!strcasecmp((const char *)en, name) && !root_valid(roots[r], (const char *)a.payload + off, rd32(en + PATH))) e = -EILSEQ;
        }
        off += rd32(en + PATH);
    }
    if (!e) e = mkdirs(root, RC_MACHINE);
    if (e) { free(a.data); return e; }
    off = 0;
    for (uint32_t i = 0; !e && i < a.count; i++) {
        const uint8_t *en = a.entries + i * ENTRY;
        e = restore_entry(root, en, a.payload + off, &seen);
        off += rd32(en + PATH);
    }
    /* The other hive slot would outrank the restored one. */
    for (unsigned i = 0; !e && seen && i < RC_SLOTS; i++) {
        if (seen & (1u << i)) continue;
        char p[512];
        if (!(e = full(p, sizeof(p), root, slots[i])) && unlink(p) && errno != ENOENT) e = -errno;
    }
    free(a.data);
    return e;
}

/* The Win32 machine hive is damaged when slot files exist and none is a
 * valid committed slot: its loader would refuse to start the broker. */
static bool hive_damaged(const char *root)
{
    if (active_slot(root) >= 0) return false;
    for (unsigned i = 0; i < RC_SLOTS; i++) {
        char p[512]; struct stat st;
        if (!full(p, sizeof(p), root, slots[i]) && !stat(p, &st)) return true;
    }
    return false;
}
/* The newest valid backup's hive slot replaces the damaged ones (set aside
 * as .bad); only before the broker loads the hive. Not inlined: the SDK's
 * 4 KiB frame limit holds for each of the two path-heavy frames. */
__attribute__((noinline)) static int restore_hive(const char *root, const struct rc_backup *list, int n, unsigned *restored, char from[16])
{
    char dir[512], path[600];
    int e = full(dir, sizeof(dir), root, RC_BACKUP_DIR); if (e) return e;
    for (int b = 0; b < n; b++) {
        if (!list[b].valid) continue;
        snprintf(path, sizeof(path), "%s/%s", dir, list[b].name);
        struct archive a;
        if (archive_open(path, &a, NULL)) continue;
        size_t off = 0; int done = 0; uint64_t sequence;
        for (uint32_t i = 0; !done && i < a.count; i++) {
            const uint8_t *en = a.entries + i * ENTRY; uint32_t size = rd32(en + PATH);
            for (unsigned k = 0; !done && k < RC_SLOTS; k++) {
                if (strcasecmp((const char *)en, slots[k]) || !slot_valid(a.payload + off, size, &sequence)) continue;
                for (unsigned j = 0; !e && j < RC_SLOTS; j++) {
                    char live[600], kept[620];
                    if ((e = full(live, sizeof(live), root, slots[j]))) break;
                    snprintf(kept, sizeof(kept), "%s.bad", live);
                    unlink(kept);
                    if (rename(live, kept) && errno != ENOENT) unlink(live);
                }
                char live[600];
                if (!e && !(e = full(live, sizeof(live), root, slots[k]))) e = write_new(live, a.payload + off, size);
                if (!e) { *restored |= RC_HIVE; if (!from[0]) memcpy(from, list[b].name, 16); }
                done = 1;
            }
            off += size;
        }
        free(a.data);
        if (done || e) return e;
    }
    return 0;
}
static int autorestore(const char *root, unsigned *restored, char from[16], bool hive)
{
    *restored = 0; from[0] = 0;
    unsigned damaged;
    int e = local_scan(root, &damaged);
    if (!e && hive && hive_damaged(root)) damaged |= RC_HIVE;
    if (e || !damaged) return e;
    struct rc_backup list[RC_MAX_BACKUPS + 4];
    int n = local_list(root, list, RC_MAX_BACKUPS + 4);
    if (n < 0) return n;
    char dir[512], path[600];
    if ((e = full(dir, sizeof(dir), root, RC_BACKUP_DIR))) return e;
    for (unsigned r = 0; r < RC_ROOTS; r++) {
        if (!(damaged & (1u << r))) continue;
        char name[PATH]; snprintf(name, sizeof(name), RC_MACHINE "/%s.reg", roots[r]);
        for (int b = 0; b < n && !(*restored & (1u << r)); b++) {
            if (!list[b].valid) continue;
            snprintf(path, sizeof(path), "%s/%s", dir, list[b].name);
            struct archive a;
            if (archive_open(path, &a, NULL)) continue;
            size_t off = 0;
            for (uint32_t i = 0; i < a.count; i++) {
                const uint8_t *en = a.entries + i * ENTRY;
                uint32_t size = rd32(en + PATH);
                if (!strcasecmp((const char *)en, name) && root_valid(roots[r], (const char *)a.payload + off, size)) {
                    /* The damaged file is set aside, then the good copy installed. */
                    char live[600], kept[620];
                    if ((e = full(live, sizeof(live), root, name))) break;
                    snprintf(kept, sizeof(kept), "%s.bad", live);
                    unlink(kept);
                    if (rename(live, kept)) unlink(live);
                    e = install_root(root, r, a.payload + off, size);
                    if (!e) { *restored |= 1u << r; if (!from[0]) memcpy(from, list[b].name, 16); }
                    break;
                }
                off += size;
            }
            free(a.data);
            if (e) return e;
        }
    }
    if (damaged & RC_HIVE) e = restore_hive(root, list, n, restored, from);
    return e;
}
static int local_autorestore(const char *root, unsigned *restored, char from[16])
{ return autorestore(root, restored, from, false); }
int rc_autorestore_start(const char *root, unsigned *restored, char from[16])
{ return autorestore(root, restored, from, true); }

/* ---- the next-start restore (the broker's) ---- */
int rc_stage_restore(const char *root, const char *name)
{
    char dir[512], path[600];
    int e = full(dir, sizeof(dir), root, RC_BACKUP_DIR); if (e) return e;
    if (!backup_name(name)) return -EINVAL;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    struct archive a;
    if ((e = archive_open(path, &a, NULL))) return e;
    free(a.data);
    char marker[600], text[16];
    if ((e = full(marker, sizeof(marker), root, PENDING))) return e;
    int n = snprintf(text, sizeof(text), "%s\n", name);
    struct stat st;
    return stat(marker, &st) ? write_new(marker, text, (size_t)n) : replace(marker, text, (size_t)n);
}
int rc_pending(const char *root, char name[16])
{
    name[0] = 0;
    char marker[600]; uint8_t *text = NULL; size_t n = 0;
    int e = full(marker, sizeof(marker), root, PENDING);
    if (!e) e = read_all(marker, &text, &n, 15);
    if (e) return e == -ENOENT ? 0 : e;
    while (n && (text[n - 1] == '\n' || text[n - 1] == '\r')) n--;
    memcpy(name, text, n); name[n] = 0;
    free(text);
    if (!backup_name(name)) { name[0] = 0; return -EILSEQ; }
    return 1;
}
int rc_apply_pending(const char *root, char applied[16])
{
    applied[0] = 0;
    char name[16], marker[600];
    int pending = rc_pending(root, name), e = full(marker, sizeof(marker), root, PENDING);
    if (e) return e;
    if (!pending) return 0;
    /* A damaged or vanished backup is dropped (never a restart loop); an I/O
     * error keeps the request for the next start. */
    e = pending < 0 ? pending : local_restore(root, name);
    if (!e || e == -EILSEQ || e == -ENOENT || e == -EINVAL) {
        if (unlink(marker) && errno != ENOENT && !e) e = -errno;
        if (!e) memcpy(applied, name, 16);
    }
    return e;
}

/* ---- the broker's wire records (apps/registry/checker.c) ---- */
void rc_wire_put(const struct rc_backup *b, uint8_t out[RC_WIRE])
{
    memset(out, 0, RC_WIRE);
    memcpy(out, b->name, 16); memcpy(out + 16, &b->created, 8);
    wr32(out + 24, b->files); wr32(out + 28, b->bytes); wr32(out + 32, (b->started ? 1u : 0) | (b->valid ? 2u : 0));
    memcpy(out + 36, b->comment, RC_COMMENT);
}
void rc_wire_get(struct rc_backup *b, const uint8_t in[RC_WIRE])
{
    memset(b, 0, sizeof(*b));
    memcpy(b->name, in, 16); b->name[15] = 0; memcpy(&b->created, in + 16, 8);
    b->files = rd32(in + 24); b->bytes = rd32(in + 28);
    b->started = rd32(in + 32) & 1; b->valid = (rd32(in + 32) & 2) != 0;
    memcpy(b->comment, in + 36, RC_COMMENT); b->comment[RC_COMMENT - 1] = 0;
}

#if RC_REMOTE
/* One request to the registry broker: a 32-byte header (version 1, then
 * op-specific words) and an optional tail; errno comes back as -errno. */
static struct reg_remote broker = { .fd = -1 };
static int ask(unsigned op, const void *tail, size_t tail_bytes, uint8_t *out, size_t cap, size_t *used)
{
    uint8_t in[32 + 64];
    if (tail_bytes > 64) return -EINVAL;
    memset(in, 0, 32); wr32(in, 1);
    if (tail_bytes) memcpy(in + 32, tail, tail_bytes);
    size_t got = 0;
    if (reg_registry_rpc(&broker, op, in, 32 + tail_bytes, out, cap, &got)) return -(errno ? errno : EIO);
    if (got < 32 || rd32(out) != 1) return -EIO;
    if (used) *used = got;
    return 0;
}
static int remote_list(struct rc_backup *out, unsigned max)
{
    uint8_t *reply = malloc(RB_BODY_MAX); size_t got = 0;
    if (!reply) return -ENOMEM;
    int e = ask(RB_CHECKER_LIST, NULL, 0, reply, RB_BODY_MAX, &got);
    unsigned count = e ? 0 : rd32(reply + 4);
    if (!e && (count > 100 || got != 32 + (size_t)count * RC_WIRE)) e = -EIO;
    unsigned n = 0;
    for (; !e && n < count && n < max; n++) rc_wire_get(&out[n], reply + 32 + n * RC_WIRE);
    free(reply);
    return e ? e : (int)n;
}
static int remote_made(unsigned op, const char *tail, size_t bytes, int *made, struct rc_backup *out)
{
    uint8_t reply[32 + RC_WIRE]; size_t got = 0;
    int e = ask(op, tail, bytes, reply, sizeof(reply), &got);
    int was = !e && rd32(reply + 4);
    if (!e && was && got != sizeof(reply)) e = -EIO;
    if (made) *made = !e && was;
    if (!e && was && out) rc_wire_get(out, reply + 32);
    return e;
}
#endif

/* ---- public calls: the engine, or on CiukiOS the broker ---- */
int rc_list(const char *root, struct rc_backup *out, unsigned max)
{
#if RC_REMOTE
    if (!root || !*root) return remote_list(out, max);
#endif
    return local_list(root, out, max);
}
int rc_backup(const char *root, const char *comment, int64_t now, bool started, struct rc_backup *made)
{
#if RC_REMOTE
    if (!root || !*root) {
        char tail[RC_COMMENT]; memset(tail, 0, sizeof(tail));
        if (comment) strncpy(tail, comment, RC_COMMENT - 1);
        int was = 0; int e = remote_made(RB_CHECKER_BACKUP, tail, sizeof(tail), &was, made);
        return e ? e : was ? 0 : -EIO;
    }
#endif
    return local_backup(root, comment, now, started, made);
}
int rc_daily(const char *root, int64_t now, int utc_offset_minutes, int *made)
{
#if RC_REMOTE
    if (!root || !*root) { (void)now; (void)utc_offset_minutes; return remote_made(RB_CHECKER_DAILY, NULL, 0, made, NULL); }
#endif
    return local_daily(root, now, utc_offset_minutes, made);
}
int rc_scan(const char *root, unsigned *damaged)
{
#if RC_REMOTE
    if (!root || !*root) {
        struct rc_status s; int e = rc_status(root, &s);
        *damaged = e ? 0 : s.damaged;
        return e;
    }
#endif
    return local_scan(root, damaged);
}
int rc_restore(const char *root, const char *name)
{
#if RC_REMOTE
    if (!root || !*root) {
        char tail[16]; memset(tail, 0, sizeof(tail));
        if (!backup_name(name)) return -EINVAL;
        memcpy(tail, name, strlen(name));
        uint8_t reply[32]; return ask(RB_CHECKER_RESTORE, tail, sizeof(tail), reply, sizeof(reply), NULL);
    }
#endif
    return local_restore(root, name);
}
int rc_autorestore(const char *root, unsigned *restored, char from[16])
{
#if RC_REMOTE
    if (!root || !*root) {
        uint8_t reply[48]; size_t got = 0;
        int e = ask(RB_CHECKER_FIX, NULL, 0, reply, sizeof(reply), &got);
        if (!e && got != sizeof(reply)) e = -EIO;
        *restored = e ? 0 : rd32(reply + 4); from[0] = 0;
        if (!e) { memcpy(from, reply + 32, 16); from[15] = 0; }
        return e;
    }
#endif
    return local_autorestore(root, restored, from);
}
int rc_status(const char *root, struct rc_status *s)
{
    memset(s, 0, sizeof(*s));
#if RC_REMOTE
    if (!root || !*root) {
        uint8_t reply[80]; size_t got = 0;
        int e = ask(RB_CHECKER_STATUS, NULL, 0, reply, sizeof(reply), &got);
        if (!e && got != sizeof(reply)) e = -EIO;
        if (e) return e;
        s->damaged = rd32(reply + 4); s->restored = rd32(reply + 8);
        memcpy(s->from, reply + 32, 16); memcpy(s->applied, reply + 48, 16); memcpy(s->pending, reply + 64, 16);
        s->from[15] = s->applied[15] = s->pending[15] = 0;
        return 0;
    }
#endif
    int e = local_scan(root, &s->damaged);
    if (!e && rc_pending(root, s->pending) < 0) s->pending[0] = 0;
    return e;
}
