/* ECMA-119 Level 1-3 and Joliet, read only. See f4-acceptance.md sources.
 * No persistent block buffer: all media bytes use the shared cache.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/fs_ro.h>
#define ISO_BLOCK 2048u
#define ISO_DESCRIPTORS 128u
#define ISO_SECTIONS 128u
struct iso_state { uint32_t blocks; uint64_t root; uint8_t joliet; };
struct iso_record {
    struct fs_ro_node node;
    uint32_t extent, bytes;
    uint8_t length, extended, flags, unit, gap, name_length;
    uint8_t identifier[222];
};
static uint16_t be16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)be16(p) << 16 | be16(p + 2); }
static int both16(const uint8_t *p, uint16_t *out)
{ *out = fs_rd16(p); return *out == be16(p + 2) ? 0 : -FS_EIO; }
static int both32(const uint8_t *p, uint32_t *out)
{ *out = fs_rd32(p); return *out == be32(p + 4) ? 0 : -FS_EIO; }
static int block(struct fs_ro_volume *v, uint64_t lba, uint8_t *buf)
{
    struct iso_state *s = v->state;
    if (lba >= v->dev->capacity || (s->blocks && lba >= s->blocks)) return -FS_EIO;
    return cache_read(v->cache, v->dev, lba, buf);
}
static int record(struct fs_ro_volume *v, uint64_t id, struct iso_record *r)
{
    uint8_t buf[ISO_BLOCK]; int e = block(v, id / ISO_BLOCK, buf); if (e) return e;
    unsigned off = (unsigned)(id % ISO_BLOCK); const uint8_t *p = buf + off;
    memset(r, 0, sizeof(*r));
    if (!p[0]) return -FS_ENOENT;
    unsigned length = p[0];
    if (length < 34 || length > ISO_BLOCK - off || !p[32] || p[32] > length - 33 ||
        33u + p[32] + !(p[32] & 1) > length) return -FS_EIO;
    uint16_t seq;
    if (both32(p + 2, &r->extent) || both32(p + 10, &r->bytes) || both16(p + 28, &seq) || seq != 1) return -FS_EIO;
    r->length = (uint8_t)length; r->extended = p[1]; r->flags = p[25];
    r->unit = p[26]; r->gap = p[27]; r->name_length = p[32];
    memcpy(r->identifier, p + 33, r->name_length);
    if ((r->flags & 0x60) || (!r->unit && r->gap) || ((r->flags & 2) && (r->unit || r->gap || (r->flags & 0x80)))) return -FS_EIO;
    uint64_t blocks = ((uint64_t)r->bytes + ISO_BLOCK - 1) / ISO_BLOCK;
    uint64_t extra = r->unit && blocks ? ((blocks - 1) / r->unit) * r->gap : 0;
    uint64_t start = (uint64_t)r->extent + r->extended;
    struct iso_state *s = v->state;
    if (start > s->blocks || blocks + extra > s->blocks - start) return -FS_EIO;
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
    if (dev->sector_size != ISO_BLOCK || start > dev->capacity || dev->capacity - start <= 16) return -FS_EINVAL;
    uint8_t data[ISO_BLOCK]; int e = cache_read(cache, dev, start + 16, data);
    if (e) return e;
    return !memcmp(data + 1, "CD001", 5) && data[6] == 1 ? 0 : -FS_EINVAL;
}
static int descriptor(struct fs_ro_volume *v, uint64_t lba, const uint8_t *b, uint8_t joliet, uint64_t *root)
{
    uint32_t count; uint16_t size, set, sequence;
    if (both32(b + 80, &count) || both16(b + 128, &size) || both16(b + 120, &set) ||
        both16(b + 124, &sequence) || !count || size != ISO_BLOCK || set != 1 || sequence != 1 || count > v->dev->capacity || b[881]!=1) return -FS_EIO;
    uint32_t table_bytes;
    if (both32(b+132,&table_bytes) || !table_bytes) return -FS_EIO;
    uint64_t table_blocks=((uint64_t)table_bytes+ISO_BLOCK-1)/ISO_BLOCK;
    uint32_t little=fs_rd32(b+140), big=be32(b+148);
    if (!little || !big || little>=count || big>=count || table_blocks>count-little || table_blocks>count-big) return -FS_EIO;
    struct iso_state *s = v->state; s->blocks = count; s->joliet = joliet;
    struct iso_record r; int e = record(v, lba * ISO_BLOCK + 156, &r);
    if (e || r.length != 34 || r.name_length != 1 || r.identifier[0] || !(r.flags & 2) || !r.bytes || r.bytes % ISO_BLOCK) return -FS_EIO;
    *root = r.node.id; return 0;
}
static int mount_iso(struct fs_ro_volume *v)
{
    v->diagnostic="malformed_iso9660";
    struct iso_state *s = fs_alloc(sizeof(*s)); if (!s) return -FS_ENOMEM;
    memset(s, 0, sizeof(*s)); v->state = s;
    uint64_t primary = 0, supplementary = 0; uint32_t blocks = 0; uint8_t preferred = 0;
    bool terminated = false;
    for (unsigned i = 0; i < ISO_DESCRIPTORS; i++) {
        uint64_t lba = v->start + 16 + i; uint8_t b[ISO_BLOCK];
        int e = block(v, lba, b); if (e) return e;
        if (memcmp(b + 1, "CD001", 5) || b[6] != 1) return -FS_EIO;
        if (b[0] == 255) { terminated = true; break; }
        if (b[0] == 1) {
            if (primary) return -FS_EIO;
            e = descriptor(v, lba, b, 0, &primary); if (e) return e;
            blocks = s->blocks;
        } else if (b[0] == 2 && b[88] == '%' && b[89] == '/') {
            uint8_t level = b[90] == '@' ? 1 : b[90] == 'C' ? 2 : b[90] == 'E' ? 3 : 0;
            if (!level || b[7]) { v->diagnostic = "malformed_joliet"; return -FS_EIO; }
            uint64_t root; e = descriptor(v, lba, b, level, &root);
            if (e || (blocks && s->blocks != blocks)) { v->diagnostic = "malformed_joliet"; return -FS_EIO; }
            if (level > preferred) { preferred = level; supplementary = root; }
        } else if (b[0] > 3) return -FS_EIO;
    }
    if (!primary || !terminated) return -FS_EIO;
    s->blocks = blocks; s->joliet = preferred; s->root = supplementary ? supplementary : primary;
    v->diagnostic = preferred ? "joliet" : "iso9660";
    return 0;
}
static void unmount_iso(struct fs_ro_volume *v) { fs_free(v->state); v->state = 0; }
const struct fs_ro_ops iso9660_ops = { "iso9660", probe, mount_iso, unmount_iso,
    root_node, lookup, readdir, read_file, metadata, generation };
