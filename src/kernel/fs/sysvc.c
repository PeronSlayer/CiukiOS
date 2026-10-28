/* sec-sysvc: SYSTEM services for administrator operations and the log model
 * (sysvc.h; user-security-kernel.md "SYSTEM services and logs"; research in
 * docs/validation/sec-sysvc.md).
 *
 * Windows XP keeps its event logs in SYSTEM-owned files that only the Event
 * Log service opens; Event Viewer reads them through that service, and
 * administrators may read and clear them (Event Logging Security, Eventlog
 * key, XP's RestrictGuestAccess for Guests). CiukiOS follows that model for
 * C:\SYSTEM\LOGS: SYSTEM writes, administrators read through LIST/READ, no
 * file access for anyone else (the kernel rule table: "read nonsecret
 * diagnostics via API; no raw backing access"). Programs that are not SYSTEM
 * keep their logs and crash reports per user (contract A1-10: Local
 * Settings\Application Data of the owner), reached through the same calls so
 * the caller never needs its profile path.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/abi.h> /* before the C library headers (FS_HOST): its macros win */
#include "sysvc.h"
#include "vfs.h"
#include "fat.h"
#include <ciuki/security.h>
#ifdef FS_HOST
#include <stdio.h>
#define sysvc_log(...) do { printf(__VA_ARGS__); putchar('\n'); } while (0)
#else
#include <ciuki/kernel.h>
#include <ciuki/storage.h>
#define sysvc_log(...) klog(__VA_ARGS__)
#endif
/* Weak: standalone filesystem harnesses link no session table. */
extern int security_session_profile(uint32_t, struct ciuki_security_profile *) __attribute__((weak));

static struct vfs *attached;
static unsigned attached_drive = 2;
void sysvc_attach(struct vfs *v, unsigned drive) { attached = v; attached_drive = drive < 26 ? drive : 2; }
static struct vfs *system_vfs(unsigned *drive)
{
    if (attached) { *drive = attached_drive; return vfs_drive_present(attached, attached_drive) ? attached : 0; }
#ifndef FS_HOST
    struct storage *s = storage_get();
    if (s && s->ready && !s->stopped && !s->setup_mode && vfs_drive_present(&s->vfs, 2)) { *drive = 2; return &s->vfs; }
#endif
    return 0;
}
int sysvc_error(int e)
{
    /* FS codes are the native errno values; internal VFS states are not. */
    if (e >= 0) return e;
    return e < -FS_EUCLEAN ? -EIO : e;
}
bool sysvc_admin(const struct security_subject *s)
{
    const struct ciuki_credential *c = s ? security_subject_credential(s) : 0;
    if (!c || !security_subject_live(s) || c->group_count > CIUKI_SECURITY_GROUP_MAX) return false;
    if (c->uid == CIUKI_UID_SYSTEM && !c->session_id) return true;
    for (unsigned i = 0; i < c->group_count; i++)
        if (c->groups[i] == CIUKI_GID_ADMINISTRATORS) return true;
    return false;
}
int sysvc_table(struct vfs_table *t, struct security_subject *bound, unsigned *drive)
{
    struct vfs *v = system_vfs(drive);
    if (!v) return -ENODEV;
    return sysvc_error(bound ? vfs_table_init_subject(v, t, *drive, bound) : vfs_table_init(v, t, *drive));
}
/* "X:" + a + b + c; a separator goes before a part that has none. */
int sysvc_path(char out[FS_PATH_BYTES], unsigned drive, const char *a, const char *b, const char *c)
{
    const char *parts[3] = { a, b, c };
    size_t at = 0;
    out[at++] = (char)('A' + drive); out[at++] = ':';
    for (unsigned i = 0; i < 3; i++) {
        if (!parts[i]) continue;
        size_t n = strlen(parts[i]);
        if (at + n + 2 > FS_PATH_BYTES) return -ENAMETOOLONG;
        if (parts[i][0] != '/') out[at++] = '/';
        memcpy(out + at, parts[i], n); at += n;
    }
    out[at] = 0;
    return 0;
}

/* ---- names ---- */
static bool name_char(char c)
{ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; }
/* 8.3 of [A-Za-z0-9_-]: what every log writer uses (BOOT.LOG, CRASH3.TXT). */
static bool log_leaf(const char *n, size_t length)
{
    unsigned base = 0, ext = 0; bool dot = false;
    for (size_t i = 0; i < length; i++) {
        if (n[i] == '.') { if (dot || !base) return false; dot = true; }
        else if (!name_char(n[i])) return false;
        else if (dot ? ++ext > 3 : ++base > 8) return false;
    }
    return base && (!dot || ext);
}
static bool hold_leaf(const char *n, size_t length)
{
    if (length != 9 || n[0] != 'u') return false;
    for (unsigned i = 1; i < 9; i++)
        if (!((n[i] >= '0' && n[i] <= '9') || (n[i] >= 'a' && n[i] <= 'f'))) return false;
    return true;
}
static size_t bounded(const char *n)
{
    size_t i = 0;
    while (i < CIUKI_SYSTEM_LOG_NAME_MAX && n[i]) i++;
    return i;
}
/* NAME.EXT, or for MACHINE Users/u<8 hex>/NAME.EXT. */
static bool log_name(const char *n, bool machine)
{
    size_t length = n ? bounded(n) : CIUKI_SYSTEM_LOG_NAME_MAX;
    if (length == CIUKI_SYSTEM_LOG_NAME_MAX) return false;
    if (log_leaf(n, length)) return true;
    if (!machine || length < 17 || strncmp(n, "Users/", 6) || n[15] != '/') return false;
    return hold_leaf(n + 6, 9) && log_leaf(n + 16, length - 16);
}
static bool same_name(const char *a, const char *b)
{
    while (*a && *b && ((*a | 32) == (*b | 32))) { a++; b++; }
    return !*a && !*b;
}
/* Administrator maintenance reports kept with the system logs (Win98's
 * SCANDISK.LOG and SFC's log): APPEND only, through this service. */
static bool maintenance_report(const char *name)
{ return same_name(name, "SCANDISK.LOG") || same_name(name, "SFC.LOG"); }

/* ---- the folder of a scope ---- */
struct log_folder {
    char path[FS_PATH_BYTES];
    size_t existing; /* bytes of path that already exist (no creation) */
    bool as_caller;
};
static int user_folder(struct security_subject *s, unsigned drive, struct log_folder *out)
{
    const struct ciuki_credential *c = security_subject_credential(s);
    /* SYSTEM work (no session) writes the machine logs directly. */
    if (!c || !c->session_id) return -EINVAL;
    struct ciuki_security_profile p;
    if (security_session_profile && !security_session_profile(c->session_id, &p) && p.uid == c->uid &&
        p.path_bytes > 1 && p.path_bytes <= sizeof(p.path) && !p.path[p.path_bytes - 1] && p.path[0] == '/' &&
        strlen(p.path) == p.path_bytes - 1) {
        int e = sysvc_path(out->path, drive, p.path, SYSVC_USER_LOGS, 0);
        out->existing = 2 + strlen(p.path); out->as_caller = true;
        return e;
    }
    /* No committed profile: only the interim administrator session of a
     * first boot; its logs wait with the system logs, SYSTEM-owned. */
    if (!sysvc_admin(s)) return -EACCES;
    char hold[10] = "u";
    for (unsigned i = 0; i < 8; i++) hold[1 + i] = "0123456789abcdef"[(c->uid >> (28 - 4 * i)) & 15];
    int e = sysvc_path(out->path, drive, SYSVC_HOLD, hold, 0);
    out->existing = 2 + strlen(SYSVC_LOGS); out->as_caller = false;
    return e;
}
static int folder(struct security_subject *s, unsigned scope, unsigned drive, struct log_folder *out)
{
    if (scope == CIUKI_SYSTEM_LOG_USER) return user_folder(s, drive, out);
    if (scope != CIUKI_SYSTEM_LOG_MACHINE) return -EINVAL;
    if (!sysvc_admin(s)) return -EPERM;
    out->existing = 0; out->as_caller = false;
    return sysvc_path(out->path, drive, SYSVC_LOGS, 0, 0);
}
static int make_folders(struct vfs_table *t, char *path, size_t existing)
{
    size_t n = strlen(path);
    for (size_t i = existing > 3 ? existing + 1 : 4; i <= n; i++) {
        if (path[i] != '/' && path[i]) continue;
        char saved = path[i]; path[i] = 0;
        int e = vfs_mkdir(t, path, 0);
        path[i] = saved;
        if (e && e != -FS_EEXIST) return sysvc_error(e);
    }
    return 0;
}
/* A vfs_table alone is about 20 KiB: every request works in one heap area. */
struct work {
    struct vfs_table t;
    struct log_folder f;
    char path[FS_PATH_BYTES], pattern[FS_PATH_BYTES], sub[FS_PATH_BYTES];
    struct vfs_find outer, inner;
    struct fat_entry entry;
    bool table;
};
static struct work *work_new(void) { return fs_contig_alloc(sizeof(struct work)); }
static void work_free(struct work *w)
{
    if (!w) return;
    if (w->table) vfs_table_destroy(&w->t);
    fs_contig_free(w, sizeof(*w));
}
static int scope_table(struct security_subject *s, unsigned scope, struct work *w)
{
    if (!security_subject_live(s)) return -EACCES;
    unsigned drive;
    if (!system_vfs(&drive)) return -ENODEV;
    int e = folder(s, scope, drive, &w->f);
    if (!e) e = sysvc_table(&w->t, w->f.as_caller ? s : 0, &drive);
    if (!e) w->table = true;
    return e;
}

/* ---- handle helpers ---- */
static int read_at(struct vfs_table *t, int fd, uint32_t offset, void *out, size_t want, size_t *done)
{
    uint64_t position;
    *done = 0;
    int e = vfs_seek(t, fd, offset, VFS_SEEK_SET, &position);
    while (!e && *done < want) {
        size_t n = 0;
        e = vfs_read(t, fd, (uint8_t *)out + *done, want - *done, &n);
        if (!e && !n) break;
        *done += n;
    }
    return sysvc_error(e);
}
static int write_at(struct vfs_table *t, int fd, uint64_t offset, const void *data, size_t bytes)
{
    uint64_t position;
    int e = vfs_seek(t, fd, (int64_t)offset, VFS_SEEK_SET, &position);
    size_t done = 0;
    if (!e && bytes) e = vfs_write(t, fd, data, bytes, &done);
    if (!e && done != bytes) e = -FS_EIO;
    return sysvc_error(e);
}
static int handle_size(struct vfs_table *t, int fd, uint32_t *size)
{
    struct fat_entry entry; uint64_t position;
    int e = sysvc_error(vfs_handle_stat(t, fd, &entry, &position));
    if (!e && (entry.attr & FAT_ATTR_DIR)) e = -EISDIR;
    if (!e) *size = entry.size;
    return e;
}

/* ---- boot configuration ---- */
static bool boot_text(const char *text, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (!(c == '\t' || c == '\r' || c == '\n' || (c >= 32 && c < 127))) return false;
    }
    return true;
}
static int boot_put(struct vfs_table *t, int fd, const char *text, size_t length)
{
    int e = write_at(t, fd, 0, text, length);
    if (!e) e = sysvc_error(vfs_truncate(t, fd, length));
    if (!e) e = sysvc_error(vfs_commit(t, fd));
    return e;
}
int sysvc_boot_config_set(struct security_subject *s, const char *text, size_t length)
{
    const struct ciuki_credential *c = s ? security_subject_credential(s) : 0;
    if (!sysvc_admin(s)) return -EPERM;
    if (!text || length > CIUKI_BOOT_CONFIG_MAX || !boot_text(text, length)) return -EINVAL;
    struct work *w = work_new();
    if (!w) return -ENOMEM;
    unsigned drive;
    int e = sysvc_table(&w->t, 0, &drive);
    if (!e) { w->table = true; e = sysvc_path(w->path, drive, SYSVC_BOOT_CONFIG, 0, 0); }
    int fd = e ? e : vfs_open(&w->t, w->path, VFS_READ | VFS_WRITE | VFS_CREATE, VFS_DENY_WRITE, 0);
    if (fd >= 0) {
        /* The loader's file is small: keep it to restore after a failed write. */
        char previous[CIUKI_BOOT_CONFIG_MAX + 2]; size_t old = 0; uint32_t size = 0;
        e = handle_size(&w->t, fd, &size);
        bool restore = !e && size < sizeof(previous) && !read_at(&w->t, fd, 0, previous, size, &old) && old == size;
        if (!e) e = boot_put(&w->t, fd, text, length);
        if (e && restore) (void)boot_put(&w->t, fd, previous, old);
        int closed = sysvc_error(vfs_close(&w->t, fd));
        if (!e) e = closed;
    } else e = sysvc_error(fd);
    work_free(w);
    sysvc_log("[sysvc] boot configuration saved uid=%u bytes=%u result=%d", c ? c->uid : 0, (unsigned)length, e);
    return e;
}

/* ---- logs ---- */
/* The index-th log file of w->pattern's folder (prefix: the returned name's
 * folder part), counted on from *seen. ENOENT: index not reached. */
static int walk_files(struct work *w, struct vfs_find *find, const char *prefix, unsigned index, unsigned *seen,
                      char name[48], uint32_t *total)
{
    size_t p = strlen(prefix);
    int e = vfs_find_first(&w->t, w->pattern, FAT_ATTR_HIDDEN | FAT_ATTR_SYSTEM, find, &w->entry);
    while (!e) {
        size_t length = strlen(w->entry.name);
        if (!(w->entry.attr & FAT_ATTR_DIR) && log_leaf(w->entry.name, length) && p + length < CIUKI_SYSTEM_LOG_NAME_MAX &&
            (*seen)++ == index) {
            memcpy(name, prefix, p); memcpy(name + p, w->entry.name, length + 1);
            *total = w->entry.size;
            return 0;
        }
        e = vfs_find_next(&w->t, find, &w->entry);
    }
    return e == -FS_ENOENT || e == -FS_ENOTDIR ? -ENOENT : sysvc_error(e);
}
static int pattern_of(char out[FS_PATH_BYTES], const char *directory)
{
    size_t n = strlen(directory);
    if (n + 3 > FS_PATH_BYTES) return -ENAMETOOLONG;
    memcpy(out, directory, n); memcpy(out + n, "/*", 3);
    return 0;
}
int sysvc_log_list(struct security_subject *s, unsigned scope, unsigned index, char name[48], uint32_t *total)
{
    if (!name || !total) return -EINVAL;
    struct work *w = work_new();
    if (!w) return -ENOMEM;
    unsigned seen = 0;
    int e = scope_table(s, scope, w);
    if (!e) e = pattern_of(w->pattern, w->f.path);
    if (!e) e = walk_files(w, &w->outer, "", index, &seen, name, total);
    if (e == -ENOENT && scope == CIUKI_SYSTEM_LOG_MACHINE) {
        /* Then the held logs of profile-less sessions: Users/u<uid>/NAME. */
        e = sysvc_path(w->sub, w->t.drive, SYSVC_HOLD, 0, 0);
        if (!e) e = pattern_of(w->pattern, w->sub);
        int r = e ? e : vfs_find_first(&w->t, w->pattern, FAT_ATTR_DIR | FAT_ATTR_HIDDEN | FAT_ATTR_SYSTEM, &w->inner, &w->entry);
        e = -ENOENT;
        while (!r && e == -ENOENT) {
            size_t length = strlen(w->entry.name);
            if ((w->entry.attr & FAT_ATTR_DIR) && hold_leaf(w->entry.name, length)) {
                char prefix[17];
                memcpy(prefix, "Users/", 6); memcpy(prefix + 6, w->entry.name, 9); prefix[15] = '/'; prefix[16] = 0;
                size_t n = strlen(w->sub);
                memcpy(w->pattern, w->sub, n); w->pattern[n] = '/'; memcpy(w->pattern + n + 1, w->entry.name, 9);
                memcpy(w->pattern + n + 10, "/*", 3);
                e = walk_files(w, &w->outer, prefix, index, &seen, name, total);
                if (e != -ENOENT) break;
            }
            r = vfs_find_next(&w->t, &w->inner, &w->entry);
        }
        if (e == -ENOENT && r && r != -FS_ENOENT && r != -FS_ENOTDIR) e = sysvc_error(r);
    }
    work_free(w);
    return e;
}
static int log_path(const struct log_folder *f, const char *name, char out[FS_PATH_BYTES])
{
    size_t n = strlen(f->path), m = strlen(name);
    if (n + m + 2 > FS_PATH_BYTES) return -ENAMETOOLONG;
    memcpy(out, f->path, n); out[n] = '/'; memcpy(out + n + 1, name, m + 1);
    return 0;
}
int sysvc_log_read(struct security_subject *s, unsigned scope, const char *name, uint32_t offset,
                   void *out, size_t capacity, size_t *done, uint32_t *total)
{
    if (!done || !total || (capacity && !out) || capacity > CIUKI_SYSTEM_LOG_DATA_MAX ||
        !log_name(name, scope == CIUKI_SYSTEM_LOG_MACHINE)) return -EINVAL;
    *done = 0; *total = 0;
    struct work *w = work_new();
    if (!w) return -ENOMEM;
    int e = scope_table(s, scope, w);
    if (!e) e = log_path(&w->f, name, w->path);
    int fd = e ? e : vfs_open(&w->t, w->path, VFS_READ, VFS_DENY_NONE, 0);
    if (fd >= 0) {
        e = handle_size(&w->t, fd, total);
        if (!e && offset < *total) {
            size_t want = *total - offset < capacity ? *total - offset : capacity;
            e = read_at(&w->t, fd, offset, out, want, done);
        }
        int closed = sysvc_error(vfs_close(&w->t, fd));
        if (!e) e = closed;
    } else if (!e) e = fd == -FS_ENOTDIR ? -ENOENT : sysvc_error(fd);
    work_free(w);
    return e;
}
int sysvc_log_write(struct security_subject *s, unsigned scope, const char *name, bool replace,
                    uint32_t limit, const void *data, size_t bytes, uint32_t *total)
{
    if (!total || (bytes && !data) || bytes > CIUKI_SYSTEM_LOG_DATA_MAX || !log_name(name, false)) return -EINVAL;
    if (!limit) limit = CIUKI_SYSTEM_LOG_LIMIT;
    if (limit > CIUKI_SYSTEM_LOG_LIMIT_MAX) return -EINVAL;
    if (bytes > limit) return -EFBIG;
    *total = 0;
    /* System logs are SYSTEM's; an administrator only appends the
     * maintenance reports. */
    if (scope == CIUKI_SYSTEM_LOG_MACHINE && (!sysvc_admin(s) || replace || !maintenance_report(name))) return -EPERM;
    struct work *w = work_new();
    if (!w) return -ENOMEM;
    int e = scope_table(s, scope, w);
    if (!e) e = make_folders(&w->t, w->f.path, w->f.existing);
    if (!e) e = log_path(&w->f, name, w->path);
    int fd = e ? e : vfs_open(&w->t, w->path, VFS_READ | VFS_WRITE | VFS_CREATE | (replace ? VFS_TRUNCATE : 0), VFS_DENY_WRITE, 0);
    if (fd >= 0) {
        uint32_t size = 0;
        e = handle_size(&w->t, fd, &size);
        /* Past the limit the file starts over (NETSURF.LOG's rule). */
        if (!e && !replace && size + bytes > limit) { e = sysvc_error(vfs_truncate(&w->t, fd, 0)); size = 0; }
        if (!e) e = write_at(&w->t, fd, size, data, bytes);
        if (!e) e = sysvc_error(vfs_commit(&w->t, fd));
        if (!e) *total = size + (uint32_t)bytes;
        int closed = sysvc_error(vfs_close(&w->t, fd));
        if (!e) e = closed;
    } else if (!e) e = sysvc_error(fd);
    work_free(w);
    return e;
}
