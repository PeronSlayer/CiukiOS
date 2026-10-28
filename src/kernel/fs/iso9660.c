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
enum iso_field {
    ISO_READ, ISO_LENGTH, ISO_FLAGS, ISO_SEQUENCE, ISO_EXTENT, ISO_DATA_LENGTH,
    ISO_VOLUME_SIZE, ISO_BLOCK_SIZE, ISO_SET_SIZE, ISO_STRUCTURE_VERSION,
    ISO_ROOT, ISO_DESCRIPTOR_VERSION, ISO_VOLUME_FLAGS, ISO_PRIMARY, ISO_TERMINATOR,
    ISO_NAME, ISO_SECTION, ISO_DIRECTORY_BOUNDS
};
struct iso_state {
    uint64_t blocks, root, failures, mismatches, rounding;
    uint8_t joliet;
};
struct iso_record {
    struct fs_ro_node node;
    uint32_t extent, bytes;
    uint8_t length, extended, flags, unit, gap, name_length;
    uint8_t identifier[222];
};
static uint16_t be16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)be16(p) << 16 | be16(p + 2); }
static const char *descriptor_name(uint8_t type)
{
    switch (type) {
    case 0: return "boot"; case 1: return "pvd"; case 2: return "svd";
    case 3: return "partition"; case 255: return "terminator"; default: return "unknown";
    }
}
static bool log_once(struct fs_ro_volume *v, enum iso_field field, unsigned category)
{
    struct iso_state *s = v->state;
    uint64_t *mask = category == 0 ? &s->failures : category == 1 ? &s->mismatches : &s->rounding;
    uint64_t bit = 1ull << field;
    if (*mask & bit) return false;
    *mask |= bit; return true;
}
static int values(struct fs_ro_volume *v, uint64_t lba, const char *source,
                  enum iso_field field, int e, unsigned category, uint64_t le, uint64_t be,
                  uint64_t min, uint64_t max, unsigned align, uint64_t available)
{
    static const char *const names[] = {
        "read", "length", "flags_interleave", "volume_sequence_number", "extent", "data_length",
        "volume_space_size", "logical_block_size", "volume_set_size", "file_structure_version",
        "root_directory_record", "descriptor_version", "volume_flags", "primary_descriptor", "terminator",
        "identifier", "section", "directory_bounds"
    };
    if (log_once(v, field, category))
        iso_log("[optical] iso session=%llu lba=%llu descriptor=%s field=%s result=%d le=%llu be=%llu min=%llu max=%llu align=%u available=%llu reason=%s",
                (unsigned long long)v->start, (unsigned long long)lba, source, names[field], e,
                (unsigned long long)le, (unsigned long long)be, (unsigned long long)min,
                (unsigned long long)max, align, (unsigned long long)available,
                category == 0 ? "invalid" : category == 1 ? "endian_mismatch" : "directory_rounding");
    return e;
}
/* Volume set fields only catalogue a disc; Linux isofs_read_inode accepts
 * any value and period mastering tools wrote 0. Diagnose, never refuse. */
static void informative(struct fs_ro_volume *v, uint64_t lba, const char *source,
                        enum iso_field field, const uint8_t *p, uint64_t available)
{
    uint32_t le = fs_rd16(p), be = be16(p + 2);
    if (le != 1) values(v, lba, source, field, 0, 0, le, be, 1, 1, 1, available);
    else if (le != be) values(v, lba, source, field, 0, 1, le, be, 1, 1, 1, available);
}
/* Match Linux isofs: a valid BE half never rescues an impossible LE value. */
static int number(struct fs_ro_volume *v, uint64_t lba, const char *source,
                  enum iso_field field, const uint8_t *p, unsigned width, uint32_t *out,
                  uint64_t min, uint64_t max, uint64_t available)
{
    uint32_t le = width == 2 ? fs_rd16(p) : fs_rd32(p);
    uint32_t be = width == 2 ? be16(p + 2) : be32(p + 4);
    if (le < min || le > max)
        return values(v, lba, source, field, -FS_EIO, 0, le, be, min, max, 1, available);
    if (le != be) values(v, lba, source, field, 0, 1, le, be, min, max, 1, available);
    *out = le; return 0;
}
static int section_failure(struct fs_ro_volume *v, uint64_t id, const struct iso_record *first,
                           const struct iso_record *r, unsigned n, int cause)
{
    if (log_once(v, ISO_SECTION, 0))
        iso_log("[optical] iso session=%llu lba=%llu descriptor=directory_record field=section result=%d cause=%d count=%u max=%u bytes=%u align=%u flags=%u first_flags=%u unit=%u first_unit=%u gap=%u first_gap=%u name_length=%u first_name_length=%u",
                (unsigned long long)v->start, (unsigned long long)(id / ISO_BLOCK), -FS_EIO, cause,
                n, ISO_SECTIONS, r->bytes, ISO_BLOCK, r->flags, first->flags, r->unit, first->unit,
                r->gap, first->gap, r->name_length, first->name_length);
    return -FS_EIO;
}
/* fx-hlis5: RUN > 1 is the number of blocks of the same extent from LBA on
 * that the caller will read next. A cache miss then reads them with one
 * READ(10), bounded by the media and BLKDEV_READ_BYTES (cache_read_ahead,
 * cache_read_limit), instead of one packet command per 2048-byte block.
 * Windows 98's CDFS reads ahead of the application into its supplemental
 * cache (Windows 98 Resource Kit, chapter 10 "VCACHE and CDFS Supplemental
 * Cache"; chapter 26, the CD-ROM access pattern). On QEMU (image c02b6731)
 * the Half-Life menu's 1.3 MB took about 45 s. */
static int block_run(struct fs_ro_volume *v, uint64_t lba, uint32_t run, uint8_t *buf)
{
    struct iso_state *s = v->state;
    uint64_t end = s->blocks ? s->blocks : v->start + v->sectors;
    if (end > v->start + v->sectors) end = v->start + v->sectors;
    if (lba < end && run > end - lba) run = (uint32_t)(end - lba);
    if (run > BLKDEV_READ_BYTES / ISO_BLOCK) run = BLKDEV_READ_BYTES / ISO_BLOCK;
    int e = lba >= end ? -FS_EIO : run > 1 ? cache_read_ahead(v->cache, v->dev, lba, run, buf) :
            cache_read(v->cache, v->dev, lba, buf);
    if (e) return values(v, lba, "set", ISO_READ, e, 0, lba, 0, 0,
                         s->blocks ? s->blocks - 1 : v->start + v->sectors - 1, 1, v->start + v->sectors);
    return 0;
}
static int block(struct fs_ro_volume *v, uint64_t lba, uint8_t *buf)
{
    struct iso_state *s = v->state;
    int e = lba >= v->start + v->sectors || (s->blocks && lba >= s->blocks) ?
            -FS_EIO : cache_read(v->cache, v->dev, lba, buf);
    if (e) return values(v, lba, "set", ISO_READ, e, 0, lba, 0, 0,
                         s->blocks ? s->blocks - 1 : v->start + v->sectors - 1, 1, v->start + v->sectors);
    return 0;
}
static __attribute__((noinline)) int record_named(struct fs_ro_volume *v, uint64_t id, struct iso_record *r, const char *source)
{
    uint64_t lba = id / ISO_BLOCK;
    uint8_t buf[ISO_BLOCK]; int e = block(v, lba, buf);
    if (e) return e;
    unsigned off = (unsigned)(id % ISO_BLOCK); const uint8_t *p = buf + off;
    memset(r, 0, sizeof(*r));
    if (!p[0]) return -FS_ENOENT;
    unsigned length = p[0];
    if (length < 34 || length > ISO_BLOCK - off)
        return values(v, lba, source, ISO_LENGTH, -FS_EIO, 0, length, off, 34, ISO_BLOCK - off, 1, ISO_BLOCK - off);
    if (!p[32] || 33u + p[32] + !(p[32] & 1) > length)
        return values(v, lba, source, ISO_LENGTH, -FS_EIO, 0, p[32], length, 1, length - 33, 1, ISO_BLOCK - off);
    r->length = (uint8_t)length; r->extended = p[1]; r->flags = p[25];
    r->unit = p[26]; r->gap = p[27]; r->name_length = p[32];
    memcpy(r->identifier, p + 33, r->name_length);
    /* Reserved metadata bits do not alter the directory/extent layout.
     * Linux isofs ignores them; keep unsupported interleave combinations
     * separate because they change where file bytes are read. */
    if ((!r->unit && r->gap) || ((r->flags & 2) && (r->unit || r->gap || (r->flags & 0x80)))) {
        if (log_once(v, ISO_FLAGS, 0))
            iso_log("[optical] iso session=%llu lba=%llu descriptor=%s field=flags_interleave result=%d flags=%u reserved_mask=96 directory_mask=2 multi_extent_mask=128 unit=%u gap=%u",
                    (unsigned long long)v->start, (unsigned long long)lba, source, -FS_EIO, r->flags, r->unit, r->gap);
        return -FS_EIO;
    }
    if (r->flags & 0x60)
        values(v,lba,source,ISO_FLAGS,0,0,r->flags,0,0,0x9f,1,r->length);
    struct iso_state *s = v->state;
    informative(v, lba, source, ISO_SEQUENCE, p + 28, s->blocks);
    if (r->extended > s->blocks)
        return values(v, lba, source, ISO_EXTENT, -FS_EIO, 0, fs_rd32(p + 2), be32(p + 6),
                      0, s->blocks, 1, r->extended);
    e = number(v, lba, source, ISO_EXTENT, p + 2, 4, &r->extent, 0, s->blocks - r->extended, s->blocks);
    if (e) return e;
    uint64_t available = s->blocks - r->extent - r->extended;
    if (r->unit) {
        unsigned cycle = r->unit + r->gap;
        uint64_t tail = available % cycle;
        available = available / cycle * r->unit + (tail < r->unit ? tail : r->unit);
    }
    e = number(v, lba, source, ISO_DATA_LENGTH, p + 10, 4, &r->bytes,
               r->flags & 2 ? 1 : 0, available * ISO_BLOCK, available); if (e) return e;
    if ((r->flags & 2) && r->bytes % ISO_BLOCK)
        values(v, lba, source, ISO_DATA_LENGTH, 0, 2, r->bytes, be32(p + 14),
               1, available * ISO_BLOCK, ISO_BLOCK, available);
    r->node.id = id; r->node.size = r->bytes;
    r->node.attr = FS_RO_READONLY | (r->flags & 2 ? FS_RO_DIRECTORY : 0) | (r->flags & 1 ? 2 : 0);
    if (r->name_length == 1 && r->identifier[0] <= 1) {
        memcpy(r->node.name, r->identifier[0] ? ".." : ".", r->identifier[0] ? 3 : 2);
        return 0;
    }
    uint16_t units[256]; unsigned n = 0;
    if (s->joliet && (r->name_length & 1))
        return values(v, lba, source, ISO_NAME, -FS_EILSEQ, 0, r->name_length, s->joliet, 2, 222, 2, r->length - 33);
    for (unsigned i = 0; i < r->name_length; i += s->joliet ? 2 : 1) {
        uint16_t ch = s->joliet ? be16(r->identifier + i) : r->identifier[i];
        if (!ch || ch == '/' || ch == '\\' || (ch >= 0xd800 && ch <= 0xdfff))
            return values(v, lba, source, ISO_NAME, -FS_EILSEQ, 0, ch, i, 1, 0xffff, s->joliet ? 2 : 1, r->name_length);
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
    e = path_from_ucs2(units, n, r->node.name, sizeof(r->node.name));
    if (!e) e = path_validate_name(r->node.name, units, &n);
    if (e) return values(v, lba, source, ISO_NAME, e, 0, r->name_length, n, 1, sizeof(r->node.name) - 1, 1, sizeof(r->node.name));
    return 0;
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
        (a->flags & ~0xe0u) == (b->flags & ~0xe0u) && a->unit == b->unit && a->gap == b->gap;
}
static int metadata(struct fs_ro_volume *v, uint64_t id, struct fs_ro_node *out)
{
    struct iso_record first, r; int e = record(v, id, &first); if (e) return e;
    r = first; *out = first.node;
    for (unsigned n = 1; r.flags & 0x80; n++) {
        if (n == ISO_SECTIONS || !r.bytes || (r.bytes % ISO_BLOCK)) return section_failure(v, id, &first, &r, n, 0);
        e = following(v, &id, &r); if (e) return section_failure(v, id, &first, &r, n, e);
        if (!same_section(&first, &r) || r.node.size > INT64_MAX - out->size) return section_failure(v, id, &first, &r, n, 0);
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
    /* The bound was checked against whole available blocks in record_named.
     * Widen before rounding: even UINT32_MAX must not wrap to zero. */
    uint64_t span = ((uint64_t)dir.bytes + ISO_BLOCK - 1) / ISO_BLOCK * ISO_BLOCK;
    if (offset > span) return values(v, directory / ISO_BLOCK, "directory_record", ISO_DIRECTORY_BOUNDS,
                                   -FS_EIO, 0, offset, dir.bytes, 0, span, ISO_BLOCK, span / ISO_BLOCK);
    while (offset < span) {
        struct iso_record r, first; uint64_t id = base + offset;
        e = record(v, id, &r);
        if (e == -FS_ENOENT) { offset = (offset / ISO_BLOCK + 1) * ISO_BLOCK; continue; }
        if (e) return e;
        first = r; *out = r.node; out->parent = directory;
        for (unsigned n = 1;; n++) {
            if (id + r.length > base + span)
                return values(v, id / ISO_BLOCK, "directory_record", ISO_DIRECTORY_BOUNDS,
                              -FS_EIO, 0, id + r.length, base, base, base + span, ISO_BLOCK, span / ISO_BLOCK);
            offset = id + r.length - base;
            if (!(r.flags & 0x80)) break;
            if (n == ISO_SECTIONS || !r.bytes || r.bytes % ISO_BLOCK) return section_failure(v, id, &first, &r, n, 0);
            e = following(v, &id, &r);
            if (e) return section_failure(v, id, &first, &r, n, e);
            if (id >= base + span) return values(v, id / ISO_BLOCK, "directory_record", ISO_DIRECTORY_BOUNDS,
                                                -FS_EIO, 0, id, base, base, base + span - 1, 1, span / ISO_BLOCK);
            if (!same_section(&first, &r) || out->size > INT64_MAX - r.bytes) return section_failure(v, id, &first, &r, n, 0);
            out->size += r.bytes;
        }
        c->opaque[0] = offset;
        if (first.name_length == 1 && first.identifier[0] <= 1) continue;
        return 0;
    }
    c->opaque[0] = span;
    return -FS_ENOENT;
}
static int lookup(struct fs_ro_volume *v, uint64_t dir, const char *name, struct fs_ro_node *out)
{
    struct fs_ro_cursor c = {{0}}; int e;
    while (!(e = readdir(v, dir, &c, out))) if (path_equal(name, out->name)) return 0;
    return e;
}
static __attribute__((noinline)) int copy_data(struct fs_ro_volume *v, uint64_t lba, uint32_t run, unsigned off, void *out, size_t n)
{
    uint8_t data[ISO_BLOCK]; int e=block_run(v,lba,run,data); if (!e) memcpy(out,data+off,n); return e;
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
            if (!(r.flags & 0x80) || ++section >= ISO_SECTIONS) return section_failure(v, id, &first, &r, section, 0);
            e = following(v, &id, &r); if (e || !same_section(&first, &r)) return section_failure(v, id, &first, &r, section, e);
            continue;
        }
        uint64_t index = position / ISO_BLOCK;
        uint64_t lba = (uint64_t)r.extent + r.extended + index;
        if (r.unit) lba += (index / r.unit) * r.gap;
        unsigned off = (unsigned)(position % ISO_BLOCK);
        size_t n = ISO_BLOCK - off;
        if (n > bytes) n = bytes;
        if (n > r.bytes - position) n = (size_t)(r.bytes - position);
        /* Contiguous blocks left in this section; interleaved ones one by one. */
        uint64_t left = ((uint64_t)off + (r.bytes - position) + ISO_BLOCK - 1) / ISO_BLOCK;
        uint32_t run = r.unit ? 1 : left > UINT32_MAX ? UINT32_MAX : (uint32_t)left;
        e=copy_data(v,lba,run,off,(uint8_t *)buf+*done,n); if (e) return e;
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
        iso_log("[optical] iso session=%llu descriptor=set field=%s result=%d sector_size=%u capacity=%llu min=17 max=%llu align=%u",
                (unsigned long long)start, dev->sector_size != ISO_BLOCK ? "device_sector_size" : "session_bounds", -FS_EINVAL,
                dev->sector_size, (unsigned long long)dev->capacity,
                (unsigned long long)(start < dev->capacity ? dev->capacity - start : 0), ISO_BLOCK);
        return -FS_EINVAL;
    }
    /* Identification belongs to the bounded descriptor walk: sector 16
     * may hold a boot record or a descriptor of another bridge format. */
    return 0;
}
static int descriptor(struct fs_ro_volume *v, uint64_t lba, const uint8_t *b, uint8_t joliet, uint64_t *root)
{
    const char *name = descriptor_name(b[0]); uint32_t count;
    int e = number(v, lba, name, ISO_VOLUME_SIZE, b + 80, 4, &count, lba - v->start + 1, UINT32_MAX, v->sectors);
    if (e) return e;
    uint32_t value;
    e = number(v, lba, name, ISO_BLOCK_SIZE, b + 128, 2, &value, ISO_BLOCK, ISO_BLOCK, v->sectors); if (e) return e;
    informative(v, lba, name, ISO_SET_SIZE, b + 120, v->sectors);
    informative(v, lba, name, ISO_SEQUENCE, b + 124, v->sectors);
    if (b[881] != 1) values(v, lba, name, ISO_STRUCTURE_VERSION, 0, 0, b[881], 0, 1, 1, 1, v->sectors);
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
    if (b[156] != 34 || b[188] != 1 || b[189] || !(b[181] & 2)) {
        if (log_once(v, ISO_ROOT, 0))
            iso_log("[optical] iso session=%llu lba=%llu descriptor=%s field=root_directory_record result=%d length=%u name_length=%u identifier=%u flags=%u min=34 max=34 align=1 available=%llu",
                    (unsigned long long)v->start, (unsigned long long)lba, name, -FS_EIO,
                    b[156], b[188], b[189], b[181], (unsigned long long)s->blocks);
        return -FS_EIO;
    }
    struct iso_record r; e = record_named(v, lba * ISO_BLOCK + 156, &r, name);
    if (e) return e;
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
        if (e) return values(v, lba, "set", ISO_READ, e, 0, lba, 0, v->start + 16, v->start + v->sectors - 1, 1, v->sectors);
        if (memcmp(b + 1, "CD001", 5)) continue;
        const char *name = descriptor_name(b[0]);
        iso_log("[optical] iso session=%llu lba=%llu descriptor=%s type=%u field=header result=0",
                (unsigned long long)v->start, (unsigned long long)lba, name, b[0]);
        if (b[0] == 255) {
            if (b[6] != 1) values(v, lba, name, ISO_DESCRIPTOR_VERSION, 0, 0, b[6], b[0], 1, 1, 1, v->sectors);
            terminated = true; break;
        }
        if (b[0] == 1 && !primary) {
            if (b[6] != 1) values(v, lba, name, ISO_DESCRIPTOR_VERSION, 0, 0, b[6], b[0], 1, 1, 1, v->sectors);
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
            if (b[7]) values(v, lba, name, ISO_VOLUME_FLAGS, 0, 0, b[7], 0, 0, 0, 1, v->sectors);
            uint64_t root; e = descriptor(v, lba, b, level, &root);
            if (e) { v->diagnostic = "malformed_joliet"; return e; }
            if (level > preferred) { preferred = level; supplementary = root; supplementary_blocks = s->blocks; }
        } /* Boot, partition, repeated PVD and unknown descriptors are ignored. */
    }
    if (!primary) return values(v, v->start + 16, "set", ISO_PRIMARY, -FS_EIO, 0, primary, supplementary, 1, ISO_DESCRIPTORS, 1, v->sectors);
    if (!terminated) values(v, v->start + 16, "set", ISO_TERMINATOR, 0, 0, terminated, primary / ISO_BLOCK, 1, ISO_DESCRIPTORS, 1, v->sectors);
    s->blocks = supplementary ? supplementary_blocks : blocks;
    s->joliet = preferred; s->root = supplementary ? supplementary : primary;
    v->diagnostic = preferred ? "joliet" : "iso9660";
    return 0;
}
static void unmount_iso(struct fs_ro_volume *v) { fs_free(v->state); v->state = 0; }
const struct fs_ro_ops iso9660_ops = { "iso9660", probe, mount_iso, unmount_iso,
    root_node, lookup, readdir, read_file, metadata, generation, 0 };
