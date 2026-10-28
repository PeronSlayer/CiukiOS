/* sec-sysvc: the call-99 adapter of the SYSTEM services (fs/sysvc.h;
 * records and rules in abi.h, CIUKI_RECOVERY_BOOT_CONFIG/LOG/DRIVER_PACK).
 * It copies the caller's record and data into kernel memory, hands the
 * retained subject to the service and copies the result back. Package files
 * are read through the caller's own namespace and subject (the frozen whole
 * file snapshot of fdtable.c); the device observations are the NDIS gate's.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/process.h>
#include <ciuki/uaddr.h>
#include <ciuki/security.h>
#include <ciuki/elf.h>
#include <ciuki/registry.h>
#include <ciuki/files.h>
#include <ciuki/sha256.h>
#include "../fs/vfs.h"
#include "../fs/sysvc.h"
#include "../ndis/package.h"
/* Weak: the NDIS gate is optional in host harnesses (no NIC: busy, unqualified). */
extern bool ndis_qualified_package(const uint8_t hash[32], const struct pci_func *, const uint8_t seal[32]) __attribute__((weak));
extern bool ndis_package_busy(const struct pci_func *) __attribute__((weak));

static void *buffer_alloc(size_t bytes) { return bytes <= 2040 ? kzalloc(bytes ? bytes : 1) : fs_contig_alloc(bytes); }
static void buffer_free(void *p, size_t bytes) { if (!p) return; if (bytes <= 2040) kfree(p); else fs_contig_free(p, bytes); }

static int boot_config(struct security_subject *s, struct trap_frame *tf)
{
    struct ciuki_boot_config r;
    if (tf->edx != sizeof(r)) return -EINVAL;
    int e = copy_from_user(&r, tf->ecx, sizeof(r));
    if (e) return e;
    if (r.version != CIUKI_BOOT_CONFIG_VERSION || r.size != sizeof(r) || r.action != CIUKI_BOOT_CONFIG_SET ||
        r.length > CIUKI_BOOT_CONFIG_MAX) return -EINVAL;
    return sysvc_boot_config_set(s, r.text, r.length);
}

static int system_log(struct security_subject *s, struct trap_frame *tf)
{
    struct ciuki_system_log r;
    if (tf->edx != sizeof(r)) return -EINVAL;
    int e = copy_from_user(&r, tf->ecx, sizeof(r));
    if (e) return e;
    if (r.version != CIUKI_SYSTEM_LOG_VERSION || r.size != sizeof(r) || r.reserved[0] || r.reserved[1] ||
        (r.scope != CIUKI_SYSTEM_LOG_MACHINE && r.scope != CIUKI_SYSTEM_LOG_USER) ||
        r.bytes > CIUKI_SYSTEM_LOG_DATA_MAX) return -EINVAL;
    r.name[sizeof(r.name) - 1] = 0;
    if (r.action == CIUKI_SYSTEM_LOG_LIST) {
        if (r.bytes || r.buffer || r.offset || r.limit) return -EINVAL;
        char name[CIUKI_SYSTEM_LOG_NAME_MAX] = { 0 };
        e = sysvc_log_list(s, r.scope, r.index, name, &r.total);
        if (e) return e;
        memcpy(r.name, name, sizeof(r.name));
        return copy_to_user(tf->ecx, &r, sizeof(r));
    }
    if (r.index || (r.bytes && !r.buffer)) return -EINVAL;
    const uint32_t allocated = r.bytes; /* READ rewrites r.bytes with the count read */
    void *data = buffer_alloc(allocated);
    if (!data) return -ENOMEM;
    if (r.action == CIUKI_SYSTEM_LOG_READ) {
        size_t done = 0;
        if (r.limit) e = -EINVAL;
        if (!e) e = sysvc_log_read(s, r.scope, r.name, r.offset, data, r.bytes, &done, &r.total);
        if (!e && done) e = copy_to_user(r.buffer, data, (uint32_t)done);
        if (!e) { r.bytes = (uint32_t)done; e = copy_to_user(tf->ecx, &r, sizeof(r)); }
    } else if (r.action == CIUKI_SYSTEM_LOG_APPEND || r.action == CIUKI_SYSTEM_LOG_REPLACE) {
        if (r.offset || (r.action == CIUKI_SYSTEM_LOG_REPLACE && r.limit)) e = -EINVAL;
        if (!e && r.bytes) e = copy_from_user(data, r.buffer, r.bytes);
        if (!e) e = sysvc_log_write(s, r.scope, r.name, r.action == CIUKI_SYSTEM_LOG_REPLACE, r.limit, data, r.bytes, &r.total);
        if (!e) e = copy_to_user(tf->ecx, &r, sizeof(r));
    } else e = -EINVAL;
    buffer_free(data, allocated);
    return e;
}

/* ---- driver packs ---- */
struct pack_caller {
    struct process *p;
    struct security_subject *s;
    const char *root;
};
static const struct pci_func *adapter(const struct ndp_device *d)
{
    for (unsigned i = 0; i < pci_count(); i++) {
        const struct pci_func *f = pci_get(i);
        if (f && f->class_code == 2 && f->subclass == 0 && f->bus == d->bus && f->dev == d->slot && f->fn == d->function &&
            f->vendor == d->vendor && f->device == d->device && f->sub_vendor == d->sub_vendor &&
            f->sub_device == d->sub_device && f->revision == d->revision) return f;
    }
    return 0;
}
static int pack_open(void *ctx, const char *relative, struct ciuki_file *out)
{
    struct pack_caller *c = ctx;
    if (!c->p || !c->p->cwd) return -EACCES;
    char *path = kmalloc(CIUKI_DRIVER_PACK_SOURCE_BYTES + 260);
    if (!path) return -ENOMEM;
    size_t a = strlen(c->root), b = strlen(relative);
    int e = a + b + 2 > CIUKI_DRIVER_PACK_SOURCE_BYTES + 260 ? -ENAMETOOLONG : 0;
    if (!e) {
        memcpy(path, c->root, a);
        size_t at = a;
        if (!a || path[a - 1] != '/') path[at++] = '/';
        for (size_t i = 0; i <= b; i++) path[at + i] = relative[i] == '\\' ? '/' : relative[i];
        /* Read as the caller: the same namespace, cwd and retained subject. */
        e = file_image_open_as(c->p->cwd, c->s, path, true, out);
    }
    kfree(path);
    return e;
}
static int pack_present(void *ctx, const struct ndp_device *d) { (void)ctx; return adapter(d) ? 1 : 0; }
static int pack_busy(void *ctx, const struct ndp_device *d)
{
    (void)ctx; const struct pci_func *f = adapter(d);
    /* Fail closed: an unobservable host counts as busy. */
    return !f || !ndis_package_busy ? 1 : ndis_package_busy(f) ? 1 : 0;
}
static int pack_qualified(void *ctx, const struct ndp_device *d, const uint8_t hash[32], const uint8_t seal[32])
{
    (void)ctx; const struct pci_func *f = adapter(d);
    return f && ndis_qualified_package && ndis_qualified_package(hash, f, seal) ? 1 : 0;
}
static int driver_pack(struct process *p, struct security_subject *s, struct trap_frame *tf)
{
    struct ciuki_driver_pack_job *r = kzalloc(sizeof(*r));
    if (!r) return -ENOMEM;
    int e = tf->edx != sizeof(*r) ? -EINVAL : copy_from_user(r, tf->ecx, sizeof(*r));
    if (!e && (r->version != CIUKI_DRIVER_PACK_JOB_VERSION || r->size != sizeof(*r) || r->reserved)) e = -EINVAL;
    r->root[sizeof(r->root) - 1] = 0;
    struct pack_caller caller = { p, s, r->root };
    struct sysvc_pack_ops ops = { &caller, pack_open, pack_present, pack_busy, pack_qualified };
    struct ndp_manifest *m = 0; char (*sources)[192] = 0;
    if (!e && r->action == CIUKI_DRIVER_PACK_INSTALL) {
        if (r->manifest_bytes != sizeof(*m) || !r->source_bytes || r->source_bytes % CIUKI_DRIVER_PACK_SOURCE_BYTES ||
            r->source_bytes > CIUKI_DRIVER_PACK_FILES * CIUKI_DRIVER_PACK_SOURCE_BYTES || r->root[0] != '/') e = -EINVAL;
        if (!e) { m = fs_contig_alloc(sizeof(*m)); sources = fs_contig_alloc(r->source_bytes); if (!m || !sources) e = -ENOMEM; }
        if (!e) e = copy_from_user(m, r->manifest, sizeof(*m));
        if (!e) e = copy_from_user(sources, r->sources, r->source_bytes);
        if (!e && r->source_bytes != m->file_count * CIUKI_DRIVER_PACK_SOURCE_BYTES) e = -EINVAL;
        if (!e) e = sysvc_pack_install(s, &ops, m, (const char (*)[192])sources, r->reason);
        else memcpy(r->reason, "Invalid or unauthorized package plan.", 38);
    } else if (!e && r->action == CIUKI_DRIVER_PACK_REMOVE) {
        if (r->manifest || r->manifest_bytes || r->sources || r->source_bytes || r->root[0]) e = -EINVAL;
        struct ndp_device d = { r->bus, r->slot, r->function, r->vendor, r->device, r->sub_vendor, r->sub_device, r->revision };
        if (!e) e = sysvc_pack_remove(s, &ops, &d, r->reason);
    } else if (!e) e = -EINVAL;
    fs_contig_free(m, sizeof(*m));
    if (sources) fs_contig_free(sources, r->source_bytes);
    /* The reason travels with a failure too; the result is the call's. */
    if (e != -EFAULT && tf->edx == sizeof(*r)) {
        int copied = copy_to_user(tf->ecx + __builtin_offsetof(struct ciuki_driver_pack_job, reason), r->reason, sizeof(r->reason));
        if (!e) e = copied;
    }
    kfree(r);
    return e;
}

/* ---- System File Checker restore (Win98 Sfc.exe "Restore file") ---- */
struct sfc_work {
    struct vfs_table t;
    unsigned drive;
    bool table;
    char target[FS_PATH_BYTES], temp[FS_PATH_BYTES], backup[FS_PATH_BYTES], scratch[FS_PATH_BYTES];
    char line[400];
    uint8_t buffer[4096];
};
static int hex_digit(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static bool fold_equal(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a >= 'a' && *a <= 'z' ? *a - 32 : *a, y = *b >= 'a' && *b <= 'z' ? *b - 32 : *b;
        if (x != y) return false;
        a++; b++;
    }
    return !*a && !*b;
}
/* "sha256 size path" (sfc.c's format): a later line replaces an earlier one. */
static bool sfc_line(const char *line, const char *path, uint8_t hash[32], uint32_t *size)
{
    if (line[0] == ';' || !line[0]) return false;
    for (unsigned i = 0; i < 64; i++) if (hex_digit(line[i]) < 0) return false;
    if (line[64] != ' ') return false;
    uint64_t n = 0; const char *p = line + 65;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') { n = n * 10 + (uint64_t)(*p - '0'); if (n > UINT32_MAX) return false; p++; }
    if (*p != ' ' || !fold_equal(p + 1, path)) return false;
    for (unsigned i = 0; i < 32; i++) hash[i] = (uint8_t)(hex_digit(line[2 * i]) << 4 | hex_digit(line[2 * i + 1]));
    *size = (uint32_t)n;
    return true;
}
static int sfc_baseline(struct sfc_work *w, const char *path, uint8_t hash[32], uint32_t *size)
{
    int e = sysvc_path(w->scratch, w->drive, "/System/SFC.LST", 0, 0);
    int fd = e ? e : vfs_open(&w->t, w->scratch, VFS_READ, VFS_DENY_WRITE, 0);
    if (fd < 0) return fd == -FS_ENOTDIR ? -ENOENT : sysvc_error(fd);
    bool found = false, overlong = false; size_t used = 0;
    for (;;) {
        size_t got = 0;
        e = sysvc_error(vfs_read(&w->t, fd, w->buffer, sizeof(w->buffer), &got));
        if (e) break;
        for (size_t i = 0; i <= got; i++) {
            bool end = i == got;
            if (end && got) break;            /* more to read */
            char c = end ? '\n' : (char)w->buffer[i];
            if (c == '\r') continue;
            if (c != '\n') { if (used + 1 < sizeof(w->line)) w->line[used++] = c; else overlong = true; continue; }
            w->line[used] = 0;
            if (!overlong && sfc_line(w->line, path, hash, size)) found = true;
            used = 0; overlong = false;
        }
        if (!got) break;
    }
    int closed = sysvc_error(vfs_close(&w->t, fd));
    if (!e) e = closed;
    return e ? e : found ? 0 : -ENOENT;
}
static int sfc_copy(struct sfc_work *w, const char *from, const char *to)
{
    int in = vfs_open(&w->t, from, VFS_READ, VFS_DENY_WRITE, 0);
    if (in < 0) return sysvc_error(in);
    int out = vfs_open(&w->t, to, VFS_WRITE | VFS_CREATE | VFS_TRUNCATE, VFS_DENY_ALL, 0), e = out < 0 ? sysvc_error(out) : 0;
    while (!e) {
        size_t got = 0, done = 0;
        e = sysvc_error(vfs_read(&w->t, in, w->buffer, sizeof(w->buffer), &got));
        if (e || !got) break;
        e = sysvc_error(vfs_write(&w->t, out, w->buffer, got, &done));
        if (!e && done != got) e = -EIO;
    }
    if (out >= 0) { if (!e) e = sysvc_error(vfs_commit(&w->t, out)); int c = sysvc_error(vfs_close(&w->t, out)); if (!e) e = c; }
    int c = sysvc_error(vfs_close(&w->t, in));
    return e ? e : c;
}
static bool sfc_path_valid(const char *path, size_t cap)
{
    size_t n = 0;
    while (n < cap && path[n]) n++;
    if (n == cap || n < 2 || path[0] != '/' || path[n - 1] == '/') return false;
    for (size_t i = 0; i < n; i++) {
        if (path[i] == '\\' || (unsigned char)path[i] < 32) return false;
        if (path[i] == '/' && (path[i + 1] == '/' || (path[i + 1] == '.' && (path[i + 2] == '/' || !path[i + 2] ||
            (path[i + 2] == '.' && (path[i + 3] == '/' || !path[i + 3])))))) return false;
    }
    return true;
}
static int sfc_restore(struct process *p, struct security_subject *s, const struct ciuki_sfc_restore *r)
{
    const struct ciuki_credential *c = security_subject_credential(s);
    if (!sysvc_admin(s)) return -EPERM;
    size_t source = 0;
    while (source < sizeof(r->source) && r->source[source]) source++;
    if (!sfc_path_valid(r->path, sizeof(r->path)) || r->source[0] != '/' || source == sizeof(r->source) ||
        !p || !p->cwd) return -EINVAL;
    struct sfc_work *w = fs_contig_alloc(sizeof(*w));
    if (!w) return -ENOMEM;
    uint8_t expected[32], digest[32]; uint32_t size = 0;
    int e = sysvc_table(&w->t, 0, &w->drive);
    if (!e) { w->table = true; e = sfc_baseline(w, r->path, expected, &size); }
    /* The original, read as the caller from its folder (a CD, a share). */
    struct ciuki_file f = { 0 }; bool open = false;
    if (!e) {
        size_t a = strlen(r->source), b = strlen(r->path);
        if (a + b + 1 > sizeof(w->scratch)) e = -ENAMETOOLONG;
        else {
            memcpy(w->scratch, r->source, a);
            if (a == 1) a = 0; /* "/" + "/System/..." */
            memcpy(w->scratch + a, r->path, b + 1);
            e = file_image_open_as(p->cwd, s, w->scratch, true, &f);
            open = !e;
        }
    }
    if (!e && f.bytes != size) e = -EILSEQ;
    if (!e) {
        struct sha256_ctx hash; sha256_init(&hash);
        for (uint32_t at = 0; !e && at < f.bytes;) {
            uint32_t want = f.bytes - at < sizeof(w->buffer) ? f.bytes - at : (uint32_t)sizeof(w->buffer);
            e = f.read(f.cookie, at, w->buffer, want);
            if (!e) { sha256_update(&hash, w->buffer, want); at += want; }
        }
        sha256_final(&hash, digest);
        if (!e && memcmp(digest, expected, 32)) e = -EILSEQ;
    }
    if (!e) e = sysvc_path(w->target, w->drive, r->path, 0, 0);
    /* The current file goes to Helpdesk\SFC first (Win98 SFC's backup). */
    struct fat_entry entry;
    if (!e && !vfs_stat(&w->t, w->target, &entry)) {
        const char *base = r->path;
        for (const char *q = r->path; *q; q++) if (*q == '/') base = q + 1;
        static const char *const folders[] = { "/System/Helpdesk", "/System/Helpdesk/SFC" };
        for (unsigned i = 0; !e && i < 2; i++) {
            e = sysvc_path(w->scratch, w->drive, folders[i], 0, 0);
            if (!e) { int m = vfs_mkdir(&w->t, w->scratch, 0); if (m && m != -FS_EEXIST) e = sysvc_error(m); }
        }
        if (!e) e = sysvc_path(w->backup, w->drive, "/System/Helpdesk/SFC", base, 0);
        if (!e) { int d = vfs_delete(&w->t, w->backup); if (d && d != -FS_ENOENT) e = sysvc_error(d); }
        if (!e) e = sfc_copy(w, w->target, w->backup);
    }
    /* The verified bytes into <path>.sfn, then one replacing rename. */
    if (!e) {
        size_t n = strlen(w->target);
        if (n + 5 > sizeof(w->temp)) e = -ENAMETOOLONG;
        else { memcpy(w->temp, w->target, n); memcpy(w->temp + n, ".sfn", 5); }
    }
    int fd = e ? e : vfs_open(&w->t, w->temp, VFS_WRITE | VFS_CREATE | VFS_TRUNCATE, VFS_DENY_ALL, 0);
    if (!e && fd < 0) e = sysvc_error(fd);
    if (fd >= 0) {
        for (uint32_t at = 0; !e && at < f.bytes;) {
            uint32_t want = f.bytes - at < sizeof(w->buffer) ? f.bytes - at : (uint32_t)sizeof(w->buffer);
            size_t done = 0;
            e = f.read(f.cookie, at, w->buffer, want);
            if (!e) e = sysvc_error(vfs_write(&w->t, fd, w->buffer, want, &done));
            if (!e && done != want) e = -EIO;
            at += want;
        }
        if (!e) e = sysvc_error(vfs_commit(&w->t, fd));
        int closed = sysvc_error(vfs_close(&w->t, fd));
        if (!e) e = closed;
        if (!e) {
            struct px_namespace *space = ((struct px_node *)p->cwd)->space;
            size_t n = strlen(r->path);
            memcpy(w->scratch, r->path, n); memcpy(w->scratch + n, ".sfn", 5);
            vfs_lock(space->vfs, 0);
            e = px_rename_locked(space, space->root, w->scratch, r->path);
            vfs_unlock(space->vfs);
        }
        if (e) (void)vfs_delete(&w->t, w->temp);
        else (void)vfs_flush_drive(&w->t, w->drive);
    }
    if (open && f.close) f.close(f.cookie);
    if (w->table) vfs_table_destroy(&w->t);
    fs_contig_free(w, sizeof(*w));
    klog("[sysvc] sfc restore uid=%u path=%s result=%d", c ? c->uid : 0, r->path, e);
    return e;
}
static int sfc_call(struct process *p, struct security_subject *s, struct trap_frame *tf)
{
    struct ciuki_sfc_restore *r = kzalloc(sizeof(*r));
    if (!r) return -ENOMEM;
    int e = tf->edx != sizeof(*r) ? -EINVAL : copy_from_user(r, tf->ecx, sizeof(*r));
    if (!e && (r->version != CIUKI_SFC_RESTORE_VERSION || r->size != sizeof(*r) || r->reserved[0] || r->reserved[1])) e = -EINVAL;
    if (!e) e = sfc_restore(p, s, r);
    kfree(r);
    return e;
}

/* ---- sec-sysvc2: SYSTEM jobs and sealed boot settings (fs/sysvc_jobs.c) ---- */
static int system_job(struct security_subject *s, struct trap_frame *tf)
{
    struct ciuki_system_job r;
    if (tf->edx != sizeof(r)) return -EINVAL;
    int e = copy_from_user(&r, tf->ecx, sizeof(r));
    if (e) return e;
    if (r.version != CIUKI_SYSTEM_JOB_VERSION || r.size != sizeof(r) || r.done ||
        r.reserved[0] || r.reserved[1] || r.reserved[2] || r.reserved[3]) return -EINVAL;
    e = sysvc_job_run(s, r.job, &r.done);
    int copied = copy_to_user(tf->ecx, &r, sizeof(r));
    return e ? e : copied;
}
static int system_setting(struct security_subject *s, struct trap_frame *tf)
{
    struct ciuki_system_setting r;
    if (tf->edx != sizeof(r)) return -EINVAL;
    int e = copy_from_user(&r, tf->ecx, sizeof(r));
    if (e) return e;
    if (r.version != CIUKI_SYSTEM_SETTING_VERSION || r.size != sizeof(r) || r.action != CIUKI_SYSTEM_SETTING_SET ||
        r.reserved[0] || r.reserved[1] || r.length > CIUKI_SYSTEM_SETTING_MAX || (r.length && !r.buffer)) return -EINVAL;
    /* The administrator check comes before the caller's text is copied. */
    if (!sysvc_admin(s)) return -EPERM;
    char *text = buffer_alloc(r.length);
    if (!text) return -ENOMEM;
    if (r.length) e = copy_from_user(text, r.buffer, r.length);
    if (!e) e = sysvc_setting_set(s, r.setting, text, r.length);
    buffer_free(text, r.length);
    return e;
}
/* ---- end sec-sysvc2 ---- */

/* Returns 1 when op is one of these services (result in *out). */
int sysvc_syscall(struct process *p, struct security_subject *s, struct trap_frame *tf, int32_t *out)
{
    switch (tf->ebx) {
    case CIUKI_RECOVERY_JOB: *out = system_job(s, tf); return 1;          /* sec-sysvc2 */
    case CIUKI_RECOVERY_SETTING: *out = system_setting(s, tf); return 1;  /* sec-sysvc2 */
    case CIUKI_RECOVERY_BOOT_CONFIG: *out = boot_config(s, tf); return 1;
    case CIUKI_RECOVERY_LOG: *out = system_log(s, tf); return 1;
    case CIUKI_RECOVERY_DRIVER_PACK: *out = driver_pack(p, s, tf); return 1;
    case CIUKI_RECOVERY_SFC_RESTORE: *out = sfc_call(p, s, tf); return 1;
    default: return 0;
    }
}
