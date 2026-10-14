/* ECMA-119 Level 1-3 and Joliet, read only. See optical-drive-binding.md sources.
 * No persistent block buffer: all media bytes use the shared cache.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/fs_ro.h>
#ifdef FS_HOST
#include <stdio.h>
#define iso_log(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#else
#define iso_log klog
#endif
#define ISO_BLOCK 2048u
#define ISO_DESCRIPTORS 128u
#define ISO_SECTIONS 128u
struct iso_state { uint64_t blocks, root; uint8_t joliet; };
struct iso_record {
    struct fs_ro_node node;
    uint32_t extent, bytes;
    uint8_t length, extended, flags, unit, gap, name_length;
    uint8_t identifier[222];
};
static uint16_t be16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)be16(p) << 16 | be16(p + 2); }
/* Linux isofs prefers the little-endian half even if mastering broke the
 * other copy. Also accept a usable big-endian half if the little one fails
 * this field's bounds/alignment. Never enlarge bounds to accommodate it. */
static bool both16(const uint8_t *p, uint16_t expected)
{ return fs_rd16(p) == expected || be16(p + 2) == expected; }
static bool both32(const uint8_t *p, uint32_t *out, uint64_t min, uint64_t max, unsigned align)
{
    uint32_t little = fs_rd32(p), big = be32(p + 4);
    if (little >= min && little <= max && !(little % align)) { *out = little; return true; }
    if (big >= min && big <= max && !(big % align)) { *out = big; return true; }
    return false;
}
static const char *descriptor_name(uint8_t type)
{
    switch (type) {
    case 0: return "boot"; case 1: return "pvd"; case 2: return "svd";
    case 3: return "partition"; case 255: return "terminator"; default: return "unknown";
    }
}
static int failure(struct fs_ro_volume *v, uint64_t lba, const char *descriptor, const char *field, int e)
{
    iso_log("[optical] iso session=%llu lba=%llu descriptor=%s field=%s result=%d",
            (unsigned long long)v->start, (unsigned long long)lba, descriptor, field, e);
    return e;
}
static int block(struct fs_ro_volume *v, uint64_t lba, uint8_t *buf)
{
    struct iso_state *s = v->state;
    if (lba >= v->start + v->sectors || (s->blocks && lba >= s->blocks)) return -FS_EIO;
    return cache_read(v->cache, v->dev, lba, buf);
}
static __attribute__((noinline)) int record_named(struct fs_ro_volume *v, uint64_t id, struct iso_record *r, const char *source)
{
    uint64_t lba = id / ISO_BLOCK;
    uint8_t buf[ISO_BLOCK]; int e = block(v, lba, buf);
    if (e) return failure(v, lba, source, "read", e);
    unsigned off = (unsigned)(id % ISO_BLOCK); const uint8_t *p = buf + off;
    memset(r, 0, sizeof(*r));
    if (!p[0]) return -FS_ENOENT;
    unsigned length = p[0];
    if (length < 34 || length > ISO_BLOCK - off || !p[32] || p[32] > length - 33 ||
        33u + p[32] + !(p[32] & 1) > length)
        return failure(v, lba, source, "length", -FS_EIO);
    r->length = (uint8_t)length; r->extended = p[1]; r->flags = p[25];
    r->unit = p[26]; r->gap = p[27]; r->name_length = p[32];
    memcpy(r->identifier, p + 33, r->name_length);
    if ((r->flags & 0x60) || (!r->unit && r->gap) || ((r->flags & 2) && (r->unit || r->gap || (r->flags & 0x80))))
        return failure(v, lba, source, "flags_interleave", -FS_EIO);
    if (!both16(p + 28, 1)) return failure(v, lba, source, "volume_sequence_number", -FS_EIO);
    struct iso_state *s = v->state;
    if (r->extended > s->blocks || !both32(p + 2, &r->extent, 0, s->blocks - r->extended, 1))
        return failure(v, lba, source, "extent", -FS_EIO);
    uint64_t available = s->blocks - r->extent - r->extended;
    if (r->unit) {
        unsigned cycle = r->unit + r->gap;
        uint64_t tail = available % cycle;
        available = available / cycle * r->unit + (tail < r->unit ? tail : r->unit);
    }
    if (!both32(p + 10, &r->bytes, r->flags & 2 ? ISO_BLOCK : 0,
                available * ISO_BLOCK, r->flags & 2 ? ISO_BLOCK : 1))
        return failure(v, lba, source, "data_length", -FS_EIO);
    r->node.id = id; r->node.size = r->bytes;
    r->node.attr = FS_RO_READONLY | (r->flags & 2 ? FS_RO_DIRECTORY : 0) | (r->flags & 1 ? 2 : 0);
    if (r->name_length == 1 && r->identifier[0] <= 1) {
        memcpy(r->node.name, r->identifier[0] ? ".." : ".", r->identifier[0] ? 3 : 2);
        return 0;
    }
    uint16_t units[256]; unsigned n = 0;
    if (s->joliet && (r->name_length & 1)) return -FS_EILSEQ;
    for (unsigned i = 0; i < r->name_length; i += s->joliet ? 2 : 1) {
        uint16_t ch = s->joliet ? be16(r->identifier + i) : r->identifier[i];
        if (!ch || ch == '/' || ch == '\\' || (ch >= 0xd800 && ch <= 0xdfff)) return -FS_EILSEQ;
        units[n++] = ch;
    }
    /* Versions are not distinct names in the case-insensitive host view.
     * Only a legal terminal ;digits is removed; collisions fail in VFS. */
    if (!(r->flags & 2)) {
        unsigned end = n, digits = 0;
        while (end && units[end - 1] >= '0' && units[end - 1] <= '9') { end--; digits++; }
        if (digits && end && units[end - 1] == ';') n = end - 1;
        if (n && units[n - 1] == '.') n--;
    }
    e = path_from_ucs2(units, n, r->node.name, sizeof(r->node.name)); if (e) return e;
    return path_validate_name(r->node.name, units, &n);
}
static int record(struct fs_ro_volume *v, uint64_t id, struct iso_record *r)
{ return record_named(v, id, r, "directory_record"); }
static int following(struct fs_ro_volume *v, uint64_t *id, struct iso_record *r)
{
    *id += r->length;
    int e = record(v, *id, r);
    if (e == -FS_ENOENT) {
        /* Zero bytes pad only to the end of one logical block. */
        *id = (*id / ISO_BLOCK + 1) * ISO_BLOCK;
        e = record(v, *id, r);
    }
    return e;
}
static bool same_section(const struct iso_record *a, const struct iso_record *b)
{
    return a->name_length == b->name_length && !memcmp(a->identifier, b->identifier, a->name_length) &&
        (a->flags & ~0x80u) == (b->flags & ~0x80u) && a->unit == b->unit && a->gap == b->gap;
}
static int metadata(struct fs_ro_volume *v, uint64_t id, struct fs_ro_node *out)
{
    struct iso_record first, r; int e = record(v, id, &first); if (e) return e;
    r = first; *out = first.node;
    for (unsigned n = 1; r.flags & 0x80; n++) {
        if (n == ISO_SECTIONS || !r.bytes || (r.bytes % ISO_BLOCK)) return -FS_EIO;
        e = following(v, &id, &r); if (e) return -FS_EIO;
        if (!same_section(&first, &r) || r.node.size > INT64_MAX - out->size) return -FS_EIO;
        out->size += r.node.size;
    }
    return 0;
}
static int root_node(struct fs_ro_volume *v, struct fs_ro_node *out)
{
    int e = metadata(v, ((struct iso_state *)v->state)->root, out);
    if (!e) { out->parent = out->id; memcpy(out->name, "/", 2); }
    return e;
}
static int readdir(struct fs_ro_volume *v, uint64_t directory, struct fs_ro_cursor *c, struct fs_ro_node *out)
{
    struct iso_record dir; int e = record(v, directory, &dir); if (e) return e;
    if (!(dir.flags & 2)) return -FS_ENOTDIR;
    uint64_t offset = c->opaque[0], base = ((uint64_t)dir.extent + dir.extended) * ISO_BLOCK;
    if (offset > dir.bytes) return -FS_EIO;
    while (offset < dir.bytes) {
        struct iso_record r, first; uint64_t id = base + offset;
        e = record(v, id, &r);
        if (e == -FS_ENOENT) { offset = (offset / ISO_BLOCK + 1) * ISO_BLOCK; continue; }
        if (e) return e;
        first = r; *out = r.node; out->parent = directory;
        for (unsigned n = 1;; n++) {
            if (id + r.length > base + dir.bytes) return -FS_EIO;
            offset = id + r.length - base;
            if (!(r.flags & 0x80)) break;
            if (n == ISO_SECTIONS || !r.bytes || r.bytes % ISO_BLOCK) return -FS_EIO;
            e = following(v, &id, &r);
            if (e || id >= base + dir.bytes || !same_section(&first, &r)) return -FS_EIO;
            if (out->size > INT64_MAX - r.bytes) return -FS_EIO;
            out->size += r.bytes;
        }
        c->opaque[0] = offset;
        if (first.name_length == 1 && first.identifier[0] <= 1) continue;
        return 0;
    }
    c->opaque[0] = dir.bytes;
    return -FS_ENOENT;
}
static int lookup(struct fs_ro_volume *v, uint64_t dir, const char *name, struct fs_ro_node *out)
{
    struct fs_ro_cursor c = {{0}}; int e;
    while (!(e = readdir(v, dir, &c, out))) if (path_equal(name, out->name)) return 0;
    return e;
}
static __attribute__((noinline)) int copy_data(struct fs_ro_volume *v, uint64_t lba, unsigned off, void *out, size_t n)
{
    uint8_t data[ISO_BLOCK]; int e=block(v,lba,data); if (!e) memcpy(out,data+off,n); return e;
}
static int read_file(struct fs_ro_volume *v, uint64_t id, uint64_t position, void *buf, size_t bytes, size_t *done)
{
    *done = 0; struct iso_record first, r; int e = record(v, id, &first); if (e) return e;
    if (first.flags & 2) return -FS_EISDIR;
    r = first;
    /* Validate the whole section chain BEFORE exposing any bytes. */
    struct fs_ro_node info; e = metadata(v, id, &info); if (e) return e;
    if (position >= info.size) return 0;
    if (bytes > info.size - position) bytes = (size_t)(info.size - position);
    unsigned section = 0;
    while (bytes) {
        if (position >= r.bytes) {
            position -= r.bytes;
            if (!(r.flags & 0x80) || ++section >= ISO_SECTIONS) return -FS_EIO;
            e = following(v, &id, &r); if (e || !same_section(&first, &r)) return -FS_EIO;
            continue;
        }
        uint64_t index = position / ISO_BLOCK;
        uint64_t lba = (uint64_t)r.extent + r.extended + index;
        if (r.unit) lba += (index / r.unit) * r.gap;
        unsigned off = (unsigned)(position % ISO_BLOCK);
        size_t n = ISO_BLOCK - off;
        if (n > bytes) n = bytes;
        if (n > r.bytes - position) n = (size_t)(r.bytes - position);
        e=copy_data(v,lba,off,(uint8_t *)buf+*done,n); if (e) return e;
        *done += n; position += n; bytes -= n;
        fs_service();
    }
    return 0;
}
static uint32_t generation(struct fs_ro_volume *v)
{
    if (v->dev->media_check && v->dev->media_check(v->dev)) return UINT32_MAX;
    return v->dev->media_generation;
}
static int probe(struct block_cache *cache, struct blkdev *dev, uint64_t start)
{
    (void)cache;
    if (dev->sector_size != ISO_BLOCK || start >= dev->capacity || dev->capacity - start <= 16) {
        iso_log("[optical] iso session=%llu descriptor=set field=%s result=%d",
                (unsigned long long)start, dev->sector_size != ISO_BLOCK ? "device_sector_size" : "session_bounds", -FS_EINVAL);
        return -FS_EINVAL;
    }
    /* Identification belongs to the bounded descriptor walk: sector 16
     * may hold a boot record or a descriptor of another bridge format. */
    return 0;
}
static int descriptor(struct fs_ro_volume *v, uint64_t lba, const uint8_t *b, uint8_t joliet, uint64_t *root)
{
    const char *name = descriptor_name(b[0]); uint32_t count;
    if (!both32(b + 80, &count, lba - v->start + 1, UINT32_MAX, 1))
        return failure(v, lba, name, "volume_space_size", -FS_EIO);
    if (!both16(b + 128, ISO_BLOCK)) return failure(v, lba, name, "logical_block_size", -FS_EIO);
    if (!both16(b + 120, 1)) return failure(v, lba, name, "volume_set_size", -FS_EIO);
    if (!both16(b + 124, 1)) return failure(v, lba, name, "volume_sequence_number", -FS_EIO);
    if (b[881] != 1) return failure(v, lba, name, "file_structure_version", -FS_EIO);
    /* We traverse directory records, never path tables. Like isofs, don't
     * refuse readable files because an unused table copy is broken. */
    struct iso_state *s = v->state;
    /* xorriso add-on sessions store their LENGTH here, although directory
     * extents are absolute. Count is not an absolute last-disc-block value.
     * Clamp before addition: supports absolute-end mastering too, without
     * ever admitting a read outside the actual media/mount end. */
    s->blocks = v->start + (count < v->sectors ? count : v->sectors); s->joliet = joliet;
    iso_log("[optical] iso session=%llu lba=%llu descriptor=%s field=volume_space_size declared=%u readable=%llu result=0",
            (unsigned long long)v->start, (unsigned long long)lba, name, count, (unsigned long long)s->blocks);
    struct iso_record r; int e = record_named(v, lba * ISO_BLOCK + 156, &r, name);
    if (e) return failure(v, lba, name, "root_directory_record", -FS_EIO);
    if (r.length != 34 || r.name_length != 1 || r.identifier[0] || !(r.flags & 2))
        return failure(v, lba, name, "root_directory_record", -FS_EIO);
    *root = r.node.id; return 0;
}
static int mount_iso(struct fs_ro_volume *v)
{
    v->diagnostic="malformed_iso9660";
    struct iso_state *s = fs_alloc(sizeof(*s)); if (!s) return -FS_ENOMEM;
    memset(s, 0, sizeof(*s)); v->state = s;
    uint64_t primary = 0, supplementary = 0, blocks = 0, supplementary_blocks = 0; uint8_t preferred = 0;
    bool terminated = false;
    for (unsigned i = 0; i < ISO_DESCRIPTORS; i++) {
        uint64_t lba = v->start + 16 + i; uint8_t b[ISO_BLOCK];
        /* Descriptor bounds use the media/session tail, independently of
         * either hierarchy's volume size. Extents remain absolute disc LBAs,
         * including files inherited from an earlier session. */
        if (16u + i >= v->sectors) break;
        int e = cache_read(v->cache, v->dev, lba, b);
        if (e) return failure(v, lba, "set", "read", e);
        if (memcmp(b + 1, "CD001", 5)) continue;
        const char *name = descriptor_name(b[0]);
        iso_log("[optical] iso session=%llu lba=%llu descriptor=%s type=%u field=header result=0",
                (unsigned long long)v->start, (unsigned long long)lba, name, b[0]);
        if (b[0] == 255) {
            if (b[6] != 1) return failure(v, lba, name, "descriptor_version", -FS_EIO);
            terminated = true; break;
        }
        if (b[0] == 1 && !primary) {
            if (b[6] != 1) return failure(v, lba, name, "descriptor_version", -FS_EIO);
            e = descriptor(v, lba, b, 0, &primary); if (e) return e;
            blocks = s->blocks;
            memcpy(v->volume_label,b+40,32); v->volume_label[32]=0;
            for (unsigned j=0;j<32;j++) if ((uint8_t)v->volume_label[j]<32 || (uint8_t)v->volume_label[j]>126) v->volume_label[j]='?';
            for (unsigned j=32;j && v->volume_label[j-1]==' ';j--) v->volume_label[j-1]=0;
            /* FNV-1a over PVD volume/set IDs, size, creation/modification dates.
             * Independent of drive assignment and media_generation. */
            uint64_t identity=14695981039346656037ull;
            for (unsigned j=40;j<88;j++) identity=(identity^b[j])*1099511628211ull;
            for (unsigned j=190;j<318;j++) identity=(identity^b[j])*1099511628211ull;
            for (unsigned j=813;j<847;j++) identity=(identity^b[j])*1099511628211ull;
            v->volume_identity=identity;
        } else if (b[0] == 2 && b[6] == 1 && b[88] == '%' && b[89] == '/') {
            uint8_t level = b[90] == '@' ? 1 : b[90] == 'C' ? 2 : b[90] == 'E' ? 3 : 0;
            if (!level) continue; /* an unimplemented character set */
            if (b[7]) { v->diagnostic = "malformed_joliet"; return failure(v, lba, name, "volume_flags", -FS_EIO); }
            uint64_t root; e = descriptor(v, lba, b, level, &root);
            if (e) { v->diagnostic = "malformed_joliet"; return e; }
            if (level > preferred) { preferred = level; supplementary = root; supplementary_blocks = s->blocks; }
        } /* Boot, partition, repeated PVD and unknown descriptors are ignored. */
    }
    if (!primary) return failure(v, v->start + 16, "set", "primary_descriptor", -FS_EIO);
    if (!terminated) return failure(v, v->start + 16, "set", "terminator", -FS_EIO);
    s->blocks = supplementary ? supplementary_blocks : blocks;
    s->joliet = preferred; s->root = supplementary ? supplementary : primary;
    v->diagnostic = preferred ? "joliet" : "iso9660";
    return 0;
}
static void unmount_iso(struct fs_ro_volume *v) { fs_free(v->state); v->state = 0; }
const struct fs_ro_ops iso9660_ops = { "iso9660", probe, mount_iso, unmount_iso,
    root_node, lookup, readdir, read_file, metadata, generation, 0 };
