/* sec-sysvc: the SYSTEM driver-pack store service (sysvc.h). The sealed
 * store C:\SYSTEM\DRIVERS (K-Q4: kernel/boot inputs are SYSTEM maintenance
 * only) changes only here, after an administrator check on the caller's
 * retained subject. The on-disk format is the importer's and the NDIS gate's
 * (apps/control/driver_pack_io.c, ndis/package_kernel.c):
 * <store>/<package>/{manifest.ndp, registry.ndp, the manifest's files}.
 * Install stages <package>.stage with transaction.ndp, copies every file from
 * the caller's package folder (read as the caller, never as SYSTEM, so the
 * service cannot carry a file the administrator could not read), verifies the
 * bytes it wrote against the sealed manifest and renames the stage into place
 * (a shorter name: FAT renames it within its own slots). Removal records
 * remove.ndp, a copy of the manifest, before manifest.ndp goes, so an
 * interrupted removal is never loaded again and the next request finishes it.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/abi.h> /* before the C library headers (FS_HOST): its macros win */
#include "sysvc.h"
#include "vfs.h"
#include "fat.h"
#include "../ndis/package.h"
#include <ciuki/elf.h>
#include <ciuki/security.h>
#include <ciuki/sha256.h>
#ifdef FS_HOST
#include <stdio.h>
#define pack_log(...) do { printf(__VA_ARGS__); putchar('\n'); } while (0)
#else
#include <ciuki/kernel.h>
#define pack_log(...) klog(__VA_ARGS__)
#endif
_Static_assert(sizeof(struct ndp_manifest) == CIUKI_DRIVER_PACK_MANIFEST_BYTES, "driver pack manifest ABI");
_Static_assert(NDP_FILES == CIUKI_DRIVER_PACK_FILES, "driver pack file bound");
#define COPY_BYTES 4096u

/* One heap area per request (a vfs_table alone is about 20 KiB). path is the
 * helpers' scratch; callers keep their own paths in stage/final/pattern. */
struct store {
    struct vfs_table t;
    unsigned drive;
    bool table;
    char root[FS_PATH_BYTES], path[FS_PATH_BYTES], pattern[FS_PATH_BYTES];
    char stage_path[FS_PATH_BYTES], final_path[FS_PATH_BYTES];
    char stage[80], found[64], removing[64];
    struct vfs_find find;
    struct fat_entry entry;
    struct ndp_manifest scratch;
    uint8_t buffer[COPY_BYTES];
};
static void reason_set(char reason[SYSVC_REASON], const char *text)
{
    size_t n = strlen(text);
    if (n >= SYSVC_REASON) n = SYSVC_REASON - 1;
    memcpy(reason, text, n); reason[n] = 0;
}
static int store_open(struct store **out)
{
    struct store *st = fs_contig_alloc(sizeof(*st));
    *out = st;
    if (!st) return -ENOMEM;
    int e = sysvc_table(&st->t, 0, &st->drive);
    if (!e) { st->table = true; e = sysvc_path(st->root, st->drive, SYSVC_DRIVERS, 0, 0); }
    return e;
}
static void store_close(struct store *st)
{
    if (!st) return;
    if (st->table) { (void)vfs_flush_drive(&st->t, st->drive); vfs_table_destroy(&st->t); }
    fs_contig_free(st, sizeof(*st));
}
static int entry_path(const struct store *st, const char *directory, const char *name, char out[FS_PATH_BYTES])
{
    size_t a = strlen(st->root), b = directory ? strlen(directory) : 0, c = name ? strlen(name) : 0;
    if (a + b + c + 3 > FS_PATH_BYTES) return -ENAMETOOLONG;
    memcpy(out, st->root, a);
    if (directory) { out[a++] = '/'; memcpy(out + a, directory, b); a += b; }
    if (name) { out[a++] = '/'; memcpy(out + a, name, c); a += c; }
    out[a] = 0;
    return 0;
}
static bool suffix(const char *name, const char *end)
{
    size_t n = strlen(name), m = strlen(end);
    return n > m && !memcmp(name + n - m, end, m);
}
static bool equal(const char *a, const char *b)
{
    while (*a && *b && ((*a | 32) == (*b | 32))) { a++; b++; }
    return !*a && !*b;
}
/* One whole record: a valid manifest image (manifest, remove, transaction). */
static int read_record(struct store *st, const char *directory, const char *name, struct ndp_manifest *m)
{
    int e = entry_path(st, directory, name, st->path);
    int fd = e ? e : vfs_open(&st->t, st->path, VFS_READ, VFS_DENY_WRITE, 0);
    if (fd < 0) return fd == -FS_ENOTDIR ? -ENOENT : sysvc_error(fd);
    struct fat_entry entry; uint64_t position; size_t done = 0, got = 0;
    e = sysvc_error(vfs_handle_stat(&st->t, fd, &entry, &position));
    if (!e && entry.size != sizeof(*m)) e = -EINVAL;
    while (!e && done < sizeof(*m)) {
        e = sysvc_error(vfs_read(&st->t, fd, (uint8_t *)m + done, sizeof(*m) - done, &got));
        if (!e && !got) e = -EIO;
        done += got;
    }
    int closed = sysvc_error(vfs_close(&st->t, fd));
    if (!e) e = closed;
    return e ? e : ndp_manifest_valid(m) ? 0 : -EINVAL;
}
static int write_new(struct store *st, const char *directory, const char *name, const void *data, size_t bytes)
{
    int e = entry_path(st, directory, name, st->path);
    int fd = e ? e : vfs_open(&st->t, st->path, VFS_WRITE | VFS_CREATE | VFS_EXCLUSIVE, VFS_DENY_ALL, 0);
    if (fd < 0) return sysvc_error(fd);
    size_t done = 0;
    e = bytes ? sysvc_error(vfs_write(&st->t, fd, data, bytes, &done)) : 0;
    if (!e && done != bytes) e = -EIO;
    if (!e) e = sysvc_error(vfs_commit(&st->t, fd));
    int closed = sysvc_error(vfs_close(&st->t, fd));
    return e ? e : closed;
}
static int remove_entry(struct store *st, const char *directory, const char *name)
{
    int e = entry_path(st, directory, name, st->path);
    if (!e) e = vfs_delete(&st->t, st->path);
    return e == -FS_ENOENT || e == -FS_ENOTDIR ? 0 : sysvc_error(e);
}
/* Every file the manifest names, then the folder: rmdir never removes an
 * unexpected file. manifest.ndp goes first so the gate never loads a
 * partly removed package; the marker (remove.ndp or transaction.ndp) last. */
static int cleanup(struct store *st, const char *directory, const struct ndp_manifest *m)
{
    int r = remove_entry(st, directory, "manifest.ndp");
    for (unsigned i = 0; i < m->file_count; i++) { int e = remove_entry(st, directory, m->files[i].name); if (!r) r = e; }
    int e = remove_entry(st, directory, "registry.ndp");
    if (!r) r = e;
    if (r) return r;
    if ((r = remove_entry(st, directory, "transaction.ndp")) || (r = remove_entry(st, directory, "remove.ndp"))) return r;
    r = entry_path(st, directory, 0, st->path);
    return r ? r : sysvc_error(vfs_rmdir(&st->t, st->path));
}

enum found { FOUND_NONE, FOUND_INSTALLED, FOUND_REMOVING };
/* The device's package folder (st->found): installed (manifest.ndp), or an
 * interrupted removal (remove.ndp, or the importer's older <package>.remove
 * folder). An installed package wins over a removal of the same device; two
 * installed packages for one device are contradictory (EEXIST). The found
 * record is left in st->scratch. */
static int find_package(struct store *st, const struct ndp_device *d, enum found *found)
{
    unsigned installed = 0;
    *found = FOUND_NONE; st->found[0] = st->removing[0] = 0;
    int e = entry_path(st, 0, "*", st->pattern);
    if (!e) e = vfs_find_first(&st->t, st->pattern, FAT_ATTR_DIR | FAT_ATTR_HIDDEN | FAT_ATTR_SYSTEM, &st->find, &st->entry);
    while (!e) {
        char name[64];
        size_t n = strlen(st->entry.name);
        bool candidate = (st->entry.attr & FAT_ATTR_DIR) && n < sizeof(name) && st->entry.name[0] != '.' &&
                         !suffix(st->entry.name, ".stage");
        if (candidate) {
            memcpy(name, st->entry.name, n + 1);
            enum found kind = FOUND_NONE;
            if (suffix(name, ".remove")) { if (!read_record(st, name, "manifest.ndp", &st->scratch)) kind = FOUND_REMOVING; }
            else if (!read_record(st, name, "remove.ndp", &st->scratch)) kind = FOUND_REMOVING;
            else if (!read_record(st, name, "manifest.ndp", &st->scratch)) kind = FOUND_INSTALLED;
            if (kind != FOUND_NONE && !memcmp(&st->scratch.device, d, sizeof(*d))) {
                if (kind == FOUND_INSTALLED && !installed++) memcpy(st->found, name, n + 1);
                if (kind == FOUND_REMOVING && !st->removing[0]) memcpy(st->removing, name, n + 1);
            }
        }
        e = vfs_find_next(&st->t, &st->find, &st->entry);
    }
    if (e == -FS_ENOENT || e == -FS_ENOTDIR) e = 0;
    e = sysvc_error(e);
    if (!e && installed > 1) e = -EEXIST;
    if (!e && installed) { *found = FOUND_INSTALLED; e = read_record(st, st->found, "manifest.ndp", &st->scratch); }
    else if (!e && st->removing[0]) {
        *found = FOUND_REMOVING; memcpy(st->found, st->removing, sizeof(st->found));
        e = read_record(st, st->found, suffix(st->found, ".remove") ? "manifest.ndp" : "remove.ndp", &st->scratch);
    }
    return e;
}

/* ndp_verify reader over the staged folder, one handle kept per file. */
struct stage_reader {
    struct store *st;
    char name[64];
    int fd;
};
static int stage_read(void *ctx, const char *name, uint32_t offset, void *out, size_t capacity, size_t *got)
{
    struct stage_reader *r = ctx;
    *got = 0;
    if (r->fd < 0 || !equal(r->name, name)) {
        if (r->fd >= 0) { (void)vfs_close(&r->st->t, r->fd); r->fd = -1; }
        size_t n = strlen(name);
        if (n >= sizeof(r->name) || entry_path(r->st, r->st->stage, name, r->st->path)) return -1;
        memcpy(r->name, name, n + 1);
        r->fd = vfs_open(&r->st->t, r->st->path, VFS_READ, VFS_DENY_WRITE, 0);
        if (r->fd < 0) return -1;
    }
    uint64_t position;
    if (vfs_seek(&r->st->t, r->fd, offset, VFS_SEEK_SET, &position)) return -1;
    return vfs_read(&r->st->t, r->fd, out, capacity, got) ? -1 : 0;
}
/* The caller's file into the stage, hashed as written. */
static int copy_in(struct store *st, const struct sysvc_pack_ops *ops, const char *source, const struct ndp_file *file)
{
    struct ciuki_file in;
    int e = ops->open(ops->ctx, source, &in);
    if (e) return e < 0 ? e : -EIO;
    if (in.bytes != file->bytes) { if (in.close) in.close(in.cookie); return -EINVAL; }
    e = entry_path(st, st->stage, file->name, st->path);
    int fd = e ? e : vfs_open(&st->t, st->path, VFS_WRITE | VFS_CREATE | VFS_EXCLUSIVE, VFS_DENY_ALL, 0);
    if (fd < 0) { if (in.close) in.close(in.cookie); return sysvc_error(fd); }
    struct sha256_ctx hash; uint8_t digest[32];
    sha256_init(&hash);
    e = 0;
    for (uint32_t at = 0; !e && at < in.bytes;) {
        uint32_t want = in.bytes - at < COPY_BYTES ? in.bytes - at : COPY_BYTES;
        size_t done = 0;
        e = in.read(in.cookie, at, st->buffer, want);
        if (e > 0) e = -EIO;
        if (!e) e = sysvc_error(vfs_write(&st->t, fd, st->buffer, want, &done));
        if (!e && done != want) e = -EIO;
        if (!e) { sha256_update(&hash, st->buffer, want); at += want; }
    }
    if (in.close) in.close(in.cookie);
    sha256_final(&hash, digest);
    if (!e && memcmp(digest, file->hash, 32)) e = -EILSEQ;
    if (!e) e = sysvc_error(vfs_commit(&st->t, fd));
    int closed = sysvc_error(vfs_close(&st->t, fd));
    return e ? e : closed;
}
static bool source_valid(const char *s)
{
    size_t n = 0;
    while (n < CIUKI_DRIVER_PACK_SOURCE_BYTES && s[n]) n++;
    return n < CIUKI_DRIVER_PACK_SOURCE_BYTES && ndp_relative(s);
}
static int stage_build(struct store *st, const struct sysvc_pack_ops *ops, const struct ndp_manifest *m,
                       const char (*sources)[192])
{
    int e = write_new(st, st->stage, "transaction.ndp", m, sizeof(*m));
    for (unsigned i = 0; !e && i < m->file_count; i++) e = copy_in(st, ops, sources[i], &m->files[i]);
    if (!e) e = write_new(st, st->stage, "registry.ndp", m->parameters, m->parameter_count * sizeof(m->parameters[0]));
    if (!e) e = write_new(st, st->stage, "manifest.ndp", m, sizeof(*m));
    if (!e) {
        struct stage_reader r = { st, "", -1 };
        e = ndp_verify(m, &m->device, stage_read, &r) ? 0 : -EILSEQ;
        if (r.fd >= 0) (void)vfs_close(&st->t, r.fd);
    }
    if (!e) e = remove_entry(st, st->stage, "transaction.ndp");
    return e;
}
/* The final folder's place: absent, or an empty folder an interrupted
 * removal left (rmdir never removes a file, so any other occupant stays). */
static int final_free(struct store *st)
{
    struct fat_entry entry;
    int e = vfs_stat(&st->t, st->final_path, &entry);
    if (e == -FS_ENOENT || e == -FS_ENOTDIR) return 0;
    if (e) return sysvc_error(e);
    return (entry.attr & FAT_ATTR_DIR) && !vfs_rmdir(&st->t, st->final_path) ? 0 : -EEXIST;
}
/* A stage this service left for the same sealed plan, never published. */
static int stage_free(struct store *st, const struct ndp_manifest *m)
{
    struct fat_entry entry;
    int e = vfs_stat(&st->t, st->stage_path, &entry);
    if (e == -FS_ENOENT || e == -FS_ENOTDIR) return 0;
    if (e) return sysvc_error(e);
    bool same = !read_record(st, st->stage, "transaction.ndp", &st->scratch) && !memcmp(st->scratch.seal, m->seal, 32) &&
                !cleanup(st, st->stage, &st->scratch);
    return same ? 0 : -EBUSY;
}
int sysvc_pack_install(struct security_subject *s, const struct sysvc_pack_ops *ops, const struct ndp_manifest *m,
                       const char (*sources)[192], char reason[SYSVC_REASON])
{
    const struct ciuki_credential *c = s ? security_subject_credential(s) : 0;
    reason_set(reason, "");
    if (!sysvc_admin(s)) { reason_set(reason, "You must be an administrator to install driver packages."); return -EPERM; }
    if (!ops || !ops->open || !ops->present || !ops->busy || !ops->qualified || !m || !sources ||
        !ndp_manifest_valid(m) || strncmp(m->reason, m->enabled ? NDP_QUALIFIED : NDP_DISABLED, sizeof(m->reason))) {
        reason_set(reason, "Invalid or unauthorized package plan."); return -EINVAL;
    }
    for (unsigned i = 0; i < m->file_count; i++)
        if (!source_valid(sources[i])) { reason_set(reason, "Unsafe source path in package plan."); return -EINVAL; }
    if (ops->present(ops->ctx, &m->device) != 1) {
        reason_set(reason, "Device changed or is quarantined. Refresh the inventory."); return -EBUSY;
    }
    if (m->enabled && (ops->qualified(ops->ctx, &m->device, m->files[m->binary].hash, m->seal) != 1 ||
                       ops->busy(ops->ctx, &m->device) != 0)) {
        reason_set(reason, "Qualification changed or the driver host is busy. Select the package again."); return -EBUSY;
    }
    struct store *st;
    enum found found;
    int e = store_open(&st);
    /* The store lives under /System, which belongs to the installed OS. */
    if (!e) { e = sysvc_error(vfs_mkdir(&st->t, st->root, 0)); if (e == -EEXIST) e = 0; }
    if (e) { reason_set(reason, "The system driver store is unavailable."); goto done; }
    e = find_package(st, &m->device, &found);
    if (e) { reason_set(reason, "The installed packages could not be read."); goto done; }
    if (found == FOUND_REMOVING && cleanup(st, st->found, &st->scratch)) {
        reason_set(reason, "An earlier removal needs to finish; remove the package again."); e = -EBUSY; goto done;
    }
    if (found == FOUND_INSTALLED) { reason_set(reason, "Remove the existing package explicitly before replacement."); e = -EEXIST; goto done; }
    size_t n = strlen(m->package);
    memcpy(st->stage, m->package, n); memcpy(st->stage + n, ".stage", 7);
    e = entry_path(st, m->package, 0, st->final_path);
    if (!e) e = entry_path(st, st->stage, 0, st->stage_path);
    if (!e && (e = final_free(st))) { reason_set(reason, "The package directory already exists; no files were replaced."); goto done; }
    if (!e && (e = stage_free(st, m))) { reason_set(reason, "An incomplete staging directory needs review before retry."); goto done; }
    if (!e) e = sysvc_error(vfs_mkdir(&st->t, st->stage_path, 0));
    if (e) { reason_set(reason, "Staging directory is unavailable; no installed package changed."); goto done; }
    e = stage_build(st, ops, m, sources);
    if (!e) e = sysvc_error(vfs_rename(&st->t, st->stage_path, st->final_path));
    if (e) {
        int failed = e;
        if (cleanup(st, st->stage, m)) reason_set(reason, "Install failed; staging cleanup needs retry. No manifest was committed.");
        else reason_set(reason, failed == -EILSEQ || failed == -EINVAL ? "A package file does not match its manifest; no installed package changed." :
                        "Install failed and was rolled back; no installed package changed.");
        goto done;
    }
    reason_set(reason, m->reason);
done:
    store_close(st);
    pack_log("[sysvc] driver pack install uid=%u package=%s result=%d", c ? c->uid : 0, m->package, e);
    return e;
}
int sysvc_pack_remove(struct security_subject *s, const struct sysvc_pack_ops *ops, const struct ndp_device *d,
                      char reason[SYSVC_REASON])
{
    const struct ciuki_credential *c = s ? security_subject_credential(s) : 0;
    reason_set(reason, "");
    if (!sysvc_admin(s)) { reason_set(reason, "You must be an administrator to remove driver packages."); return -EPERM; }
    if (!ops || !ops->present || !ops->busy || !d) return -EINVAL;
    struct store *st;
    enum found found;
    int e = store_open(&st);
    if (e) { reason_set(reason, "The system driver store is unavailable."); goto done; }
    e = find_package(st, d, &found);
    struct ndp_manifest *m = &st->scratch;
    if (e) reason_set(reason, "The installed packages could not be read.");
    else if (found == FOUND_NONE) { reason_set(reason, "No removable package found."); e = -ENOENT; }
    else if (found == FOUND_REMOVING) {
        /* manifest.ndp is gone (or the folder is the importer's tombstone):
         * the gate no longer loads it; a qualified host must still be idle. */
        if (suffix(st->found, ".remove") && m->enabled && ops->busy(ops->ctx, d) != 0) {
            reason_set(reason, "An active or quarantined package requires a successful host stop first."); e = -EBUSY;
        } else {
            e = cleanup(st, st->found, m) ? -EIO : 0;
            reason_set(reason, e ? "Removal cleanup needs retry." : "Driver package removed.");
        }
    } else if (ops->present(ops->ctx, d) != 1) {
        reason_set(reason, "Device changed or is quarantined. Refresh the inventory."); e = -EBUSY;
    } else if (m->enabled && ops->busy(ops->ctx, d) != 0) {
        /* A missing or failed observation cannot prove a qualified host inactive. */
        reason_set(reason, "An active or quarantined package requires a successful host stop first."); e = -EBUSY;
    } else {
        e = remove_entry(st, st->found, "remove.ndp");
        if (!e) e = write_new(st, st->found, "remove.ndp", m, sizeof(*m));
        if (e) reason_set(reason, "Removal failed; installed package retained.");
        else {
            e = cleanup(st, st->found, m) ? -EIO : 0;
            reason_set(reason, e ? "Package disabled; removal cleanup needs retry." : "Driver package removed. Native driver remains the default.");
        }
    }
done:
    store_close(st);
    pack_log("[sysvc] driver pack remove uid=%u result=%d", c ? c->uid : 0, e);
    return e;
}
