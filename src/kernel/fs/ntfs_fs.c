/* NTFS read-only adapter: ntfs_ro (f4-03) behind the fs_ro interface (f4-04).
 * Decisions and research sources: docs/design/f4-acceptance.md, "## f4-04".
 * All media bytes come from the shared block cache through the library's
 * read callback; the adapter owns no block cache, issues no write or flush
 * and keeps one bounded directory snapshot inside its mount workspace.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/fs_ro.h>
#include <ciuki/ntfs_ro.h>
#ifdef FS_HOST
#include <stdio.h>
#define ntfs_log(...) do { fprintf(stderr,__VA_ARGS__); fputc('\n',stderr); } while (0)
#else
#define ntfs_log klog
#endif

#define SNAP_RAW (2u * FS_RO_ENTRIES + 2u) /* every entry may carry a DOS alias entry */
#define SNAP_POOL 32768u                   /* UTF-8 bytes of one directory's names */
#define NO_NAME UINT32_MAX
#define METAFILES 24u                      /* MFT records 0-23 are NTFS system files */
#define READ_CHUNK (1024u * 1024u)         /* ntfs_ro_pread limit */
#define NS_POSIX 0u
#define NS_WIN32 1u
#define NS_DOS 2u

struct raw {
    uint64_t ref, size;
    uint32_t name, alias, attrs;
    int32_t status;
    uint8_t ns, directory, keep;
};
/* One mount: this header, then the library workspace in the same contiguous
 * allocation (page aligned; the library demands uint64 alignment). */
struct ntfs_fs {
    size_t bytes;
    struct ntfs_ro *lib;
    struct fs_ro_volume *volume;
    int io_error;
    /* Directories already counted into volume->names_omitted. */
    uint32_t counted_used;
    uint64_t counted[FS_RO_VIEWS];
    /* Snapshot of one directory, rebuilt at cursor zero. */
    uint64_t directory;
    bool valid;
    uint32_t raw_used, order_used, pool_used;
    uint32_t order[FS_RO_ENTRIES];
    struct raw raw[SNAP_RAW];
    char pool[SNAP_POOL];
};

static uint32_t workspace_offset(void) { return (uint32_t)((sizeof(struct ntfs_fs) + 63u) & ~63u); }

/* Library block callback: whole 512-byte sectors, volume relative. The first
 * device error is remembered so it is reported as the device's error, never
 * mistaken for damaged NTFS metadata. */
static int lib_read(void *opaque, uint64_t lba, uint32_t count, void *buf)
{
    struct ntfs_fs *s = opaque; struct fs_ro_volume *v = s->volume; uint8_t *p = buf;
    for (uint32_t i = 0; i < count; i++) {
        int e = cache_read(v->cache, v->dev, v->start + lba + i, p + ((size_t)i << 9));
        if (e) { if (!s->io_error) s->io_error = e; return -1; }
    }
    return 0;
}
static int result(struct ntfs_fs *s, int e)
{
    if (s->io_error) { e = s->io_error; s->io_error = 0; }
    return e;
}

static int probe(struct block_cache *cache, struct blkdev *dev, uint64_t start)
{
    if (dev->sector_size != 512 || start >= dev->capacity) return -FS_EINVAL;
    uint8_t boot[512]; int e = cache_read(cache, dev, start, boot);
    if (e) return e;
    return !memcmp(boot + 3, "NTFS    ", 8) && boot[510] == 0x55 && boot[511] == 0xaa ? 0 : -FS_EINVAL;
}
static void unmount_ntfs(struct fs_ro_volume *v)
{
    struct ntfs_fs *s = v->state;
    if (!s) return;
    if (s->lib) ntfs_ro_unmount(s->lib);
    fs_contig_free(s, s->bytes); v->state = 0;
}
/* Refusal reasons are the library's: dirty, recovery-required, hibernated,
 * unsupported geometry/limits, otherwise malformed. Nothing is repaired. */
static int mount_ntfs(struct fs_ro_volume *v)
{
    v->diagnostic = "ntfs_malformed";
    if (v->dev->sector_size != 512) { v->diagnostic = "ntfs_sector_size"; return -FS_EINVAL; }
    size_t ws = ntfs_ro_workspace_size(), total = workspace_offset() + ws;
    struct ntfs_fs *s = fs_contig_alloc(total);
    if (!s) { v->diagnostic = "ntfs_workspace"; return -FS_ENOMEM; }
    s->bytes = total; s->volume = v; v->state = s;
    enum ntfs_ro_reason reason = NTFS_RO_INVALID;
    int e = ntfs_ro_mount((uint8_t *)s + workspace_offset(), ws, lib_read, s, 512, v->sectors, &s->lib, &reason);
    if (!e) {
        if (reason==NTFS_RO_DIRTY) ntfs_log("[ntfs] dirty flag: accepted read-only after clean journal and hibernation checks");
        v->diagnostic = "ntfs"; return 0;
    }
    e = result(s, e);
    switch (reason) {
    case NTFS_RO_DIRTY: v->diagnostic = "ntfs_dirty"; return -FS_EUCLEAN;
    case NTFS_RO_RECOVERY_REQUIRED: v->diagnostic = "ntfs_recovery_required"; return -FS_EUCLEAN;
    case NTFS_RO_HIBERNATED: v->diagnostic = "ntfs_hibernated"; return -FS_EUCLEAN;
    case NTFS_RO_UNSUPPORTED: v->diagnostic = "ntfs_unsupported"; return -FS_EOPNOTSUPP;
    default: return e ? e : -FS_EIO;
    }
}
static uint32_t generation(struct fs_ro_volume *v)
{
    if (v->dev->media_check && v->dev->media_check(v->dev)) return UINT32_MAX;
    return v->dev->media_generation;
}

static void fill(struct fs_ro_node *out, uint64_t id, uint64_t parent, uint64_t size, bool directory, uint8_t flags)
{
    memset(out, 0, sizeof(*out));
    out->id = id; out->parent = parent; out->size = directory ? 0 : size;
    out->attr = (uint8_t)(FS_RO_READONLY | (flags & 0x26) | (directory ? FS_RO_DIRECTORY : 0));
}
static int root_node(struct fs_ro_volume *v, struct fs_ro_node *out)
{
    struct ntfs_fs *s = v->state; struct ntfs_ro_node n;
    if (!s) return -FS_EIO;
    s->io_error = 0;
    int e = result(s, ntfs_ro_root(s->lib, &n)); if (e) return e;
    fill(out, n.reference, n.reference, 0, true, 0); memcpy(out->name, "/", 2);
    return 0;
}
/* Validates an id (sequence number included) and reports its authoritative
 * size; unsupported or damaged content returns its error here. */
static int metadata(struct fs_ro_volume *v, uint64_t id, struct fs_ro_node *out)
{
    struct ntfs_fs *s = v->state; struct ntfs_ro_node n;
    if (!s) return -FS_EIO;
    s->io_error = 0;
    int e = result(s, ntfs_ro_open(s->lib, id, &n)); if (e) return e;
    fill(out, id, 0, n.size, n.directory, 0);
    return 0;
}

static int emit_raw(void *opaque, const struct ntfs_ro_entry *e)
{
    struct ntfs_fs *s = opaque;
    size_t length = strlen(e->name) + 1;
    if (s->raw_used == SNAP_RAW || length > SNAP_POOL - s->pool_used) return -FS_ENOSPC;
    struct raw *r = &s->raw[s->raw_used++];
    memset(r, 0, sizeof(*r));
    r->ref = e->reference; r->size = e->size; r->alias = NO_NAME;
    r->attrs = e->file_attributes; r->ns = (uint8_t)e->name_space; r->directory = e->directory != 0; r->keep = 1;
    memcpy(s->pool + s->pool_used, e->name, length);
    r->name = s->pool_used; s->pool_used += (uint32_t)length;
    return 0;
}
/* Each DOS-namespace index entry is an alias of exactly one long name of the
 * same MFT reference. An orphan is damage; two candidates cannot be told
 * apart from the index alone (hard links in one directory) and fail closed. */
static int pair_aliases(struct ntfs_fs *s)
{
    for (uint32_t i = 0; i < s->raw_used; i++) {
        if (s->raw[i].ns != NS_DOS) continue;
        uint32_t partner = UINT32_MAX, count = 0;
        for (uint32_t j = 0; j < s->raw_used; j++)
            if (j != i && s->raw[j].ref == s->raw[i].ref && (s->raw[j].ns == NS_WIN32 || s->raw[j].ns == NS_POSIX)) { partner = j; count++; }
        if (!count) return -FS_EIO;
        if (count > 1) return -FS_EOPNOTSUPP;
        s->raw[partner].alias = s->raw[i].name; s->raw[i].keep = 0;
    }
    return 0;
}
static bool counted(struct ntfs_fs *s, uint64_t directory)
{
    for (uint32_t i = 0; i < s->counted_used; i++) if (s->counted[i] == directory) return true;
    if (s->counted_used < FS_RO_VIEWS) s->counted[s->counted_used++] = directory;
    return false;
}
/* Names the VFS cannot hold (outside the BMP, characters it forbids, trailing
 * dot or space) are never shown wrongly: a valid DOS alias becomes the name,
 * otherwise the entry is omitted and counted in names_omitted, once per
 * directory for the life of the mount. DOS aliases the view would refuse are
 * dropped (the VFS then generates its own). */
static int select_entries(struct ntfs_fs *s, uint64_t directory)
{
    uint32_t omitted = 0; uint16_t units[256]; unsigned length;
    s->order_used = 0;
    for (uint32_t i = 0; i < s->raw_used; i++) {
        struct raw *r = &s->raw[i];
        if (!r->keep || (r->ref & UINT64_C(0x0000ffffffffffff)) < METAFILES) continue;
        const char *name = s->pool + r->name;
        if (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))) continue;
        char alias[40]; bool have_alias = false;
        if (r->alias != NO_NAME) have_alias = fs_ro_short_name(s->pool + r->alias, alias);
        if (path_validate_name(name, units, &length)) {
            if (!have_alias) { omitted++; continue; }
            memcpy(s->pool + r->alias, alias, strlen(alias) + 1); /* canonical, same length */
            r->name = r->alias; r->alias = NO_NAME;
        } else if (have_alias) memcpy(s->pool + r->alias, alias, strlen(alias) + 1);
        else r->alias = NO_NAME;
        if (s->order_used == FS_RO_ENTRIES) return -FS_ENOSPC;
        s->order[s->order_used++] = i;
    }
    if (!counted(s, directory)) s->volume->names_omitted += omitted;
    return 0;
}
/* Authoritative sizes come from the node (the index copy may be stale). An
 * unsupported or damaged node stays listed with a status that its lookup
 * returns; a failing device aborts the whole snapshot. */
static int resolve_nodes(struct ntfs_fs *s)
{
    for (uint32_t k = 0; k < s->order_used; k++) {
        struct raw *r = &s->raw[s->order[k]]; struct ntfs_ro_node n;
        s->io_error = 0;
        int e = ntfs_ro_open(s->lib, r->ref, &n);
        if (s->io_error) return result(s, e);
        if (!e) { r->size = n.size; r->directory = n.directory != 0; r->status = 0; }
        else { r->size = 0; r->status = e; }
    }
    return 0;
}
static int snapshot(struct fs_ro_volume *v, uint64_t directory)
{
    struct ntfs_fs *s = v->state; struct ntfs_ro_node dir = { .reference = directory, .directory = 1 };
    if (!s) return -FS_EIO;
    s->valid = false; s->raw_used = s->order_used = s->pool_used = 0; s->io_error = 0;
    int e = result(s, ntfs_ro_list(s->lib, &dir, emit_raw, s));
    if (!e) e = pair_aliases(s);
    if (!e) e = select_entries(s, directory);
    if (!e) e = resolve_nodes(s);
    if (e) return e;
    s->directory = directory; s->valid = true;
    return 0;
}
/* The cursor is an index into the snapshot of the directory, rebuilt whenever
 * enumeration starts (cursor zero) or the cached directory differs. */
static int readdir_ntfs(struct fs_ro_volume *v, uint64_t directory, struct fs_ro_cursor *c, struct fs_ro_node *out)
{
    struct ntfs_fs *s = v->state; if (!s) return -FS_EIO;
    uint64_t index = c->opaque[0];
    if (!index || !s->valid || s->directory != directory) {
        int e = snapshot(v, directory); if (e) return e;
    }
    if (index >= s->order_used) { c->opaque[0] = s->order_used; return -FS_ENOENT; }
    struct raw *r = &s->raw[s->order[index]];
    fill(out, r->ref, directory, r->size, r->directory, (uint8_t)r->attrs);
    size_t length = strlen(s->pool + r->name) + 1;
    if (length > sizeof(out->name)) return -FS_ENAMETOOLONG;
    memcpy(out->name, s->pool + r->name, length);
    if (r->alias != NO_NAME) memcpy(out->alias, s->pool + r->alias, strlen(s->pool + r->alias) + 1);
    out->status = r->status;
    c->opaque[0] = index + 1;
    return 0;
}
/* Volume-folding lookup ($UpCase) used by the VFS when its own folding finds
 * no entry (FS_RO_DRIVER_COLLATION). Long names and DOS aliases both match. */
static int lookup_ntfs(struct fs_ro_volume *v, uint64_t directory, const char *name, struct fs_ro_node *out)
{
    struct ntfs_fs *s = v->state; struct ntfs_ro_node dir = { .reference = directory, .directory = 1 }, n;
    if (!s) return -FS_EIO;
    s->io_error = 0;
    int e = result(s, ntfs_ro_lookup(s->lib, &dir, name, &n)); if (e) return e;
    fill(out, n.reference, directory, n.size, n.directory, 0);
    return 0;
}
static int read_file(struct fs_ro_volume *v, uint64_t id, uint64_t position, void *buf, size_t bytes, size_t *done)
{
    struct ntfs_fs *s = v->state; struct ntfs_ro_node node = { .reference = id }; uint8_t *out = buf;
    *done = 0; if (!s) return -FS_EIO;
    while (bytes) {
        size_t chunk = bytes > READ_CHUNK ? READ_CHUNK : bytes, got = 0;
        s->io_error = 0;
        int e = result(s, ntfs_ro_pread(s->lib, &node, position, out, chunk, &got));
        if (e) { *done = 0; return e; }
        *done += got; out += got; position += got; bytes -= got;
        if (got < chunk) break; /* end of file */
    }
    return 0;
}
const struct fs_ro_ops ntfs_ops = { "ntfs", probe, mount_ntfs, unmount_ntfs, root_node, lookup_ntfs,
    readdir_ntfs, read_file, metadata, generation, FS_RO_STREAM_SYNTAX | FS_RO_DRIVER_COLLATION };
