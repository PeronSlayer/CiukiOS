/* G14 ScanDisk engine (docs/design/startup-recovery.md section 2.4):
 * FAT12/16/32 over a partition block device, independent of the mounted
 * FAT layer. Standard: FAT census and mirror, the directory tree with one
 * ownership bit per cluster, cross-links copied ("Make copies"), lost
 * chains saved as \FOUND.nnn\FILEnnnn.CHK ("Convert to files"), invalid
 * links and cycles ended, sizes beyond the chain adjusted, reserved
 * attribute bits cleared. Thorough adds a read of every sector: bad free
 * clusters are marked, used ones moved. Without FIX nothing is written
 * (Windows 98's Preview): the same counts come from an in-memory copy.
 * The clean and hard-error bits stay the FAT layer's (its dirty repair
 * runs afterwards and recovers BOOT*.LOG reservations).
 * Microsoft fatgen103: BPB, FAT type determination, FAT entry layout,
 * FAT[1] ClnShutBitMask/HrdErrBitMask, FSInfo (0xFFFFFFFF = unknown).
 * SPDX-License-Identifier: GPL-2.0-only */
#include "scandisk.h"

extern void klog(const char *, ...) __attribute__((weak));

#define WINDOW_SECTORS 64u
#define RUN_BYTES 65536u
#define BITMAP_PAGES 1024u          /* 32 Mi clusters */
#define QUEUE_PAGES 256u            /* 131,072 directories */
#define CROSSLINKS 64u
#define CHK_MAX 10000u

struct bits { uint8_t *page[BITMAP_PAGES]; uint32_t pages; };
struct entry_loc { uint64_t lba; uint16_t offset; };
struct crosslink { struct entry_loc entry; uint32_t pred, shared, prefix, size; bool dir; };
struct vol {
    struct blkdev *dev;
    struct ciuki_scandisk *r;
    bool fix, thorough;
    unsigned type;
    uint32_t spc, cb, reserved, fats, fat_sectors, root_entries, root_sector, root_sectors;
    uint32_t data_sector, clusters, root_cluster, total, active;
    bool mirror;
    uint8_t *win; uint32_t win_first, win_count; bool win_valid, win_dirty;
    uint8_t *buf;                   /* RUN_BYTES: clusters, sectors, copies */
    struct bits owned, linked;
    uint32_t *queue[QUEUE_PAGES]; uint32_t queue_pages, queue_head, queue_tail;
    struct crosslink xl[CROSSLINKS]; unsigned nxl;
    uint32_t alloc_hint, found_dir, found_next;
    bool changed;
    int error;
};

static uint32_t mask(const struct vol *v) { return v->type == 12 ? 0xfff : v->type == 16 ? 0xffff : 0x0fffffff; }
static uint32_t bad_value(const struct vol *v) { return mask(v) - 8; }
static bool eoc(const struct vol *v, uint32_t x) { return x >= mask(v) - 7; }
static bool valid(const struct vol *v, uint32_t c) { return c >= 2 && c < v->clusters + 2; }
static uint64_t cluster_lba(const struct vol *v, uint32_t c) { return v->data_sector + (uint64_t)(c - 2) * v->spc; }

static void problem(struct vol *v, const uint8_t *e)
{
    if (v->r->first_problem[0] || !e) return;
    unsigned n = 0;
    for (unsigned i = 0; i < 8 && e[i] != ' '; i++) v->r->first_problem[n++] = (char)e[i];
    if (e[8] != ' ') { v->r->first_problem[n++] = '.'; for (unsigned i = 8; i < 11 && e[i] != ' '; i++) v->r->first_problem[n++] = (char)e[i]; }
    v->r->first_problem[n] = 0;
}

static int rd(struct vol *v, uint64_t lba, uint32_t n, void *b)
{
    int e = v->dev->read(v->dev, lba, n, b);
    return e;
}
static int wr(struct vol *v, uint64_t lba, uint32_t n, const void *b)
{
    if (!v->fix) return 0;
    v->changed = true;
    int e = v->dev->write ? v->dev->write(v->dev, lba, n, b) : -FS_EROFS;
    if (e && !v->error) v->error = e;
    return e;
}

/* ---- bitmaps ---------------------------------------------------------- */
static int bits_init(struct bits *b, uint32_t count)
{
    b->pages = (count / 8 + 4095) / 4096 + 1;
    if (b->pages > BITMAP_PAGES) return -FS_EFBIG;
    for (uint32_t i = 0; i < b->pages; i++) if (!(b->page[i] = fs_page_alloc())) return -FS_ENOMEM;
    return 0;
}
static void bits_free(struct bits *b)
{
    for (uint32_t i = 0; i < b->pages; i++) if (b->page[i]) fs_page_free(b->page[i]);
    b->pages = 0;
}
static bool bit_get(const struct bits *b, uint32_t c) { return b->page[c / 32768][(c % 32768) / 8] & (1u << (c % 8)); }
static void bit_set(struct bits *b, uint32_t c) { b->page[c / 32768][(c % 32768) / 8] |= (uint8_t)(1u << (c % 8)); }

/* ---- FAT window on the active copy ------------------------------------ */
static int win_flush(struct vol *v)
{
    if (!v->win_valid || !v->win_dirty) return 0;
    v->win_dirty = false;
    for (uint32_t f = 0; f < v->fats; f++) {
        if (f != v->active && !v->mirror) continue;
        int e = wr(v, v->reserved + (uint64_t)f * v->fat_sectors + v->win_first, v->win_count, v->win);
        if (e) return e;
    }
    return 0;
}
/* One FAT sector of `copy`, or of another copy when it cannot be read (the
 * mirror exists for this). *missing: no copy of `copy` itself was read. */
static int fat_sector(struct vol *v, uint32_t copy, uint32_t s, uint8_t *out, bool *missing)
{
    *missing = false;
    if (!rd(v, v->reserved + (uint64_t)copy * v->fat_sectors + s, 1, out)) return 0;
    *missing = true;
    for (uint32_t f = 0; f < v->fats; f++)
        if (f != copy && !rd(v, v->reserved + (uint64_t)f * v->fat_sectors + s, 1, out)) return 0;
    return -FS_EIO;
}
static int win_load(struct vol *v, uint32_t sector, uint32_t need)
{
    if (v->win_valid && sector >= v->win_first && sector + need <= v->win_first + v->win_count) return 0;
    int e = win_flush(v); if (e) return e;
    uint32_t count = WINDOW_SECTORS;
    if (sector + count > v->fat_sectors) count = v->fat_sectors - sector;
    if (count < need) return -FS_EINVAL;
    v->win_valid = false;
    if (rd(v, v->reserved + (uint64_t)v->active * v->fat_sectors + sector, count, v->win)) {
        bool missing;
        for (uint32_t i = 0; i < count; i++)
            if ((e = fat_sector(v, v->active, sector + i, v->win + i * 512, &missing))) return e;
    }
    v->win_first = sector; v->win_count = count; v->win_valid = true;
    return 0;
}
static uint32_t fat_offset(const struct vol *v, uint32_t c) { return v->type == 12 ? c + c / 2 : c * (v->type / 8); }
static int get(struct vol *v, uint32_t c, uint32_t *out)
{
    uint32_t off = fat_offset(v, c), sector = off / 512, need = v->type == 12 && off % 512 == 511 ? 2 : 1;
    int e = win_load(v, sector, need); if (e) return e;
    const uint8_t *p = v->win + (sector - v->win_first) * 512 + off % 512;
    if (v->type == 12) { uint32_t x = p[0] | (uint32_t)p[1] << 8; *out = c & 1 ? x >> 4 : x & 0xfff; }
    else if (v->type == 16) *out = fs_rd16(p);
    else *out = fs_rd32(p) & 0x0fffffff;
    return 0;
}
static int set(struct vol *v, uint32_t c, uint32_t x)
{
    uint32_t off = fat_offset(v, c), sector = off / 512, need = v->type == 12 && off % 512 == 511 ? 2 : 1;
    int e = win_load(v, sector, need); if (e) return e;
    uint8_t *p = v->win + (sector - v->win_first) * 512 + off % 512;
    if (v->type == 12) {
        uint32_t old = p[0] | (uint32_t)p[1] << 8;
        old = c & 1 ? (old & 0x000f) | (x & 0xfff) << 4 : (old & 0xf000) | (x & 0xfff);
        p[0] = (uint8_t)old; p[1] = (uint8_t)(old >> 8);
    } else if (v->type == 16) fs_wr16(p, (uint16_t)x);
    else fs_wr32(p, (fs_rd32(p) & 0xf0000000u) | (x & 0x0fffffff));
    v->win_dirty = true;
    return 0;
}

/* ---- directory entries ------------------------------------------------- */
static int entry_read(struct vol *v, const struct entry_loc *l, uint8_t out[32])
{
    int e = rd(v, l->lba, 1, v->buf); if (e) return e;
    memcpy(out, v->buf + l->offset, 32); return 0;
}
static int entry_write(struct vol *v, const struct entry_loc *l, const uint8_t in[32])
{
    if (!v->fix) return 0;
    int e = rd(v, l->lba, 1, v->buf); if (e) return e;
    memcpy(v->buf + l->offset, in, 32);
    return wr(v, l->lba, 1, v->buf);
}
static uint32_t entry_first(const struct vol *v, const uint8_t *e)
{
    return fs_rd16(e + 26) | (v->type == 32 ? (uint32_t)fs_rd16(e + 20) << 16 : 0);
}
static void entry_set_first(const struct vol *v, uint8_t *e, uint32_t c)
{
    fs_wr16(e + 26, (uint16_t)c);
    if (v->type == 32) fs_wr16(e + 20, (uint16_t)(c >> 16));
}

/* ---- allocation --------------------------------------------------------- */
static int alloc_cluster(struct vol *v, uint32_t *out)
{
    for (uint32_t n = 0; n < v->clusters; n++) {
        if (!(n & 1023)) fs_service();
        uint32_t c = 2 + (v->alloc_hint + n) % v->clusters, x;
        int e = get(v, c, &x); if (e) return e;
        if (x || bit_get(&v->owned, c)) continue;
        if ((e = set(v, c, mask(v)))) return e;
        bit_set(&v->owned, c);
        v->alloc_hint = c - 2 + 1;
        *out = c; return 0;
    }
    return -FS_ENOSPC;
}
static int zero_cluster(struct vol *v, uint32_t c)
{
    memset(v->buf, 0, v->cb);
    return wr(v, cluster_lba(v, c), v->spc, v->buf);
}

/* ---- queue of directories (first cluster, chain length) ------------------ */
static int queue_push(struct vol *v, uint32_t first, uint32_t length)
{
    uint32_t slot = v->queue_tail, page = slot / 512;
    if (page >= QUEUE_PAGES) { v->r->flags |= CIUKI_SCANDISK_TRUNCATED; return 0; }
    if (page >= v->queue_pages) {
        if (!(v->queue[page] = fs_page_alloc())) { v->r->flags |= CIUKI_SCANDISK_TRUNCATED; return 0; }
        v->queue_pages++;
    }
    v->queue[page][(slot % 512) * 2] = first; v->queue[page][(slot % 512) * 2 + 1] = length;
    v->queue_tail++;
    return 0;
}

/* ---- the chain of one entry ---------------------------------------------- */
static bool in_chain(struct vol *v, uint32_t first, uint32_t steps, uint32_t c)
{
    uint32_t x = first;
    for (uint32_t i = 0; i < steps && valid(v, x); i++) {
        if (x == c) return true;
        if (get(v, x, &x)) return false;
    }
    return false;
}
/* Returns the chain length owned for this entry; *crossed when it ended at
 * another owner's cluster. Fixes: invalid link or cycle ends the chain. */
/* prefix: clusters this entry owns before the shared one. */
static void crossed(struct vol *v, const uint8_t *e, const struct entry_loc *loc, uint32_t pred, uint32_t shared,
                    uint32_t prefix, bool dir)
{
    if (v->nxl < CROSSLINKS) v->xl[v->nxl++] = (struct crosslink){ *loc, pred, shared, prefix, fs_rd32(e + 28), dir };
    v->r->crosslinked_files++; problem(v, e);
}
static int walk_chain(struct vol *v, uint8_t *e, const struct entry_loc *loc, bool dir, uint32_t *length, bool *rewrite)
{
    uint32_t first = entry_first(v, e), c = first, len = 0;
    *length = 0;
    if (!first) return 0;
    if (!valid(v, first)) {
        /* A first cluster outside the volume: the entry keeps its name. */
        v->r->invalid_chains++; problem(v, e);
        entry_set_first(v, e, 0); if (!dir) fs_wr32(e + 28, 0);
        *rewrite = true;
        return 0;
    }
    if (bit_get(&v->owned, first)) { crossed(v, e, loc, 0, first, 0, dir); return 0; }
    for (;;) {
        if (!(len & 255)) fs_service();
        bit_set(&v->owned, c); len++;
        uint32_t next; int err = get(v, c, &next); if (err) return err;
        if (eoc(v, next)) break;
        if (!valid(v, next) || next == bad_value(v)) {
            v->r->invalid_chains++; problem(v, e);
            if ((err = set(v, c, mask(v)))) return err;
            break;
        }
        if (bit_get(&v->owned, next)) {
            if (in_chain(v, first, len, next)) {
                v->r->invalid_chains++; problem(v, e);
                if ((err = set(v, c, mask(v)))) return err;
            } else crossed(v, e, loc, c, next, len, dir);
            break;
        }
        c = next;
        if (len > v->clusters) break;
    }
    *length = len;
    return 0;
}

/* One directory's entries. root_fixed: the FAT12/16 root region. */
static int scan_directory(struct vol *v, uint32_t first, uint32_t length, bool root_fixed)
{
    uint32_t c = first, sectors = root_fixed ? v->root_sectors : v->spc, clusters = root_fixed ? 1 : length;
    uint8_t e[32];
    for (uint32_t k = 0; k < clusters; k++) {
        uint64_t base = root_fixed ? v->root_sector : cluster_lba(v, c);
        for (uint32_t s = 0; s < sectors; s++) {
            int err = rd(v, base + s, 1, v->buf); if (err) return err;
            uint8_t sector[512]; memcpy(sector, v->buf, 512);
            for (unsigned off = 0; off < 512; off += 32) {
                memcpy(e, sector + off, 32);
                if (!e[0]) return 0;                       /* end of directory */
                if (e[0] == 0xe5 || e[11] == 0x0f) continue; /* deleted, long name */
                if ((e[11] & 0x08) && !(e[11] & 0x10)) continue; /* volume label */
                if (e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' '))) continue;
                struct entry_loc loc = { base + s, (uint16_t)off };
                bool rewrite = false;
                if (e[11] & 0xc0) {
                    v->r->invalid_entries++; problem(v, e);
                    e[11] &= 0x3f; rewrite = true;
                }
                bool dir = (e[11] & 0x10) != 0;
                uint32_t len = 0;
                if ((err = walk_chain(v, e, &loc, dir, &len, &rewrite))) return err;
                if (dir) {
                    v->r->directories++; v->r->directory_clusters += len;
                    if (len && (err = queue_push(v, entry_first(v, e), len))) return err;
                } else {
                    v->r->files++; v->r->file_clusters += len;
                    uint32_t size = fs_rd32(e + 28), need = size / v->cb + (size % v->cb != 0);
                    bool crossed = v->nxl && v->xl[v->nxl - 1].entry.lba == loc.lba && v->xl[v->nxl - 1].entry.offset == loc.offset;
                    if (len < need && !crossed) {
                        v->r->size_mismatches++; problem(v, e);
                        fs_wr32(e + 28, len * v->cb); rewrite = true;
                    } else if (len > need) v->r->excess_tails++;
                }
                if (rewrite && (err = entry_write(v, &loc, e))) return err;
            }
        }
        if (root_fixed) break;
        uint32_t next; int err2 = get(v, c, &next); if (err2) return err2;
        if (k + 1 < clusters && !valid(v, next)) break;
        c = next;
    }
    return 0;
}

static int scan_tree(struct vol *v)
{
    int e;
    if (v->type == 32) {
        uint8_t root[32] = { 0 }; root[11] = 0x10;
        entry_set_first(v, root, v->root_cluster);
        struct entry_loc none = { 0, 0 };
        uint32_t len = 0; bool rewrite = false;
        if ((e = walk_chain(v, root, &none, true, &len, &rewrite))) return e;
        v->r->directory_clusters += len;
        if ((e = scan_directory(v, v->root_cluster, len, false))) return e;
    } else if ((e = scan_directory(v, 0, 0, true))) return e;
    while (v->queue_head < v->queue_tail) {
        uint32_t slot = v->queue_head++;
        uint32_t first = v->queue[slot / 512][(slot % 512) * 2], len = v->queue[slot / 512][(slot % 512) * 2 + 1];
        fs_service();
        if ((e = scan_directory(v, first, len, false))) return e;
    }
    return 0;
}

/* ---- cross-links: copy the shared tail for the later owner -------------- */
static int copy_crosslinks(struct vol *v)
{
    for (unsigned i = 0; i < v->nxl; i++) {
        struct crosslink *x = &v->xl[i];
        /* A copied directory tail would duplicate its entries' chains:
         * directories stay reported, as do the root and NO_COPY. */
        if (!v->fix || (v->r->flags & CIUKI_SCANDISK_NO_COPY) || x->dir || (!x->pred && !x->entry.lba)) continue;
        /* Copy only what the file's size still needs after its own
         * clusters (Windows 98 "Make copies"); a shorter shared tail
         * shortens the file to what exists. */
        uint32_t need = x->size / v->cb + (x->size % v->cb != 0);
        uint32_t want = need > x->prefix ? need - x->prefix : 0;
        uint32_t src = x->shared, head = 0, tail = 0, n = 0;
        int e = 0;
        uint8_t ent[32];
        if (!want) {
            if (x->pred) { if ((e = set(v, x->pred, mask(v)))) return e; }
            else {
                if ((e = entry_read(v, &x->entry, ent))) return e;
                entry_set_first(v, ent, 0); fs_wr32(ent + 28, 0);
                if ((e = entry_write(v, &x->entry, ent))) return e;
            }
            v->r->crosslinks_fixed++;
            continue;
        }
        while (valid(v, src) && n < want) {
            uint32_t copy;
            if ((e = alloc_cluster(v, &copy))) break;
            if ((e = rd(v, cluster_lba(v, src), v->spc, v->buf)) || (e = wr(v, cluster_lba(v, copy), v->spc, v->buf))) break;
            if (tail && (e = set(v, tail, copy))) break;
            if (!head) head = copy;
            tail = copy; n++;
            uint32_t next; if ((e = get(v, src, &next))) break;
            if (eoc(v, next) || !valid(v, next)) break;
            src = next;
            if (!(n & 63)) fs_service();
        }
        if (e == -FS_ENOSPC || !head) { if (e && e != -FS_ENOSPC) return e; continue; }
        if (e) return e;
        if (x->pred) { if ((e = set(v, x->pred, head))) return e; }
        if (!x->pred || n < want) {
            if ((e = entry_read(v, &x->entry, ent))) return e;
            if (!x->pred) entry_set_first(v, ent, head);
            if (n < want) { fs_wr32(ent + 28, (x->prefix + n) * v->cb); v->r->size_mismatches++; }
            if ((e = entry_write(v, &x->entry, ent))) return e;
        }
        v->r->crosslinks_fixed++;
    }
    return 0;
}

/* ---- lost chains --------------------------------------------------------- */
static void chk_name(uint8_t name[11], const char *base, unsigned n, const char *ext)
{
    memcpy(name, base, 4);
    name[4] = (uint8_t)('0' + n / 1000 % 10); name[5] = (uint8_t)('0' + n / 100 % 10);
    name[6] = (uint8_t)('0' + n / 10 % 10); name[7] = (uint8_t)('0' + n % 10);
    memcpy(name + 8, ext, 3);
}
static void dos_entry(uint8_t e[32], const uint8_t name[11], uint8_t attr, uint32_t first, uint32_t size, const struct vol *v)
{
    memset(e, 0, 32); memcpy(e, name, 11); e[11] = attr;
    uint16_t date, time; uint8_t tenths; fs_timestamp(&date, &time, &tenths);
    e[13] = tenths; fs_wr16(e + 14, time); fs_wr16(e + 16, date); fs_wr16(e + 18, date);
    fs_wr16(e + 22, time); fs_wr16(e + 24, date);
    entry_set_first(v, e, first); fs_wr32(e + 28, size);
}
/* A free slot in directory `dir` (0 = fixed root), grown by one cluster
 * when full (never the fixed root). */
static int free_slot(struct vol *v, uint32_t dir, struct entry_loc *out)
{
    bool fixed = !dir;
    uint32_t c = dir, last = dir;
    for (uint32_t k = 0; k <= v->clusters; k++) {
        uint32_t sectors = fixed ? v->root_sectors : v->spc;
        uint64_t base = fixed ? v->root_sector : cluster_lba(v, c);
        for (uint32_t s = 0; s < sectors; s++) {
            int e = rd(v, base + s, 1, v->buf); if (e) return e;
            for (unsigned off = 0; off < 512; off += 32)
                if (!v->buf[off] || v->buf[off] == 0xe5) { *out = (struct entry_loc){ base + s, (uint16_t)off }; return 0; }
        }
        if (fixed) return -FS_ENOSPC;
        last = c;
        uint32_t next; int e = get(v, c, &next); if (e) return e;
        if (eoc(v, next) || !valid(v, next)) break;
        c = next;
    }
    uint32_t grown; int e = alloc_cluster(v, &grown); if (e) return e;
    if ((e = zero_cluster(v, grown)) || (e = set(v, last, grown))) return e;
    *out = (struct entry_loc){ cluster_lba(v, grown), 0 };
    return 0;
}
static int found_directory(struct vol *v)
{
    if (v->found_dir) return 0;
    uint32_t root = v->type == 32 ? v->root_cluster : 0;
    uint8_t name[11];
    /* FOUND.000 .. FOUND.999: the first name not present in the root. */
    for (unsigned n = 0; n < 1000; n++) {
        memcpy(name, "FOUND   ", 8);
        name[8] = (uint8_t)('0' + n / 100); name[9] = (uint8_t)('0' + n / 10 % 10); name[10] = (uint8_t)('0' + n % 10);
        bool taken = false;
        uint32_t c = root;
        for (uint32_t k = 0; k <= v->clusters && !taken; k++) {
            uint32_t sectors = root ? v->spc : v->root_sectors;
            uint64_t base = root ? cluster_lba(v, c) : v->root_sector;
            bool end = false;
            for (uint32_t s = 0; s < sectors && !taken && !end; s++) {
                int e = rd(v, base + s, 1, v->buf); if (e) return e;
                for (unsigned off = 0; off < 512; off += 32) {
                    if (!v->buf[off]) { end = true; break; }
                    if (!memcmp(v->buf + off, name, 11)) { taken = true; break; }
                }
            }
            if (end || !root) break;
            uint32_t next; int e = get(v, c, &next); if (e) return e;
            if (eoc(v, next) || !valid(v, next)) break;
            c = next;
        }
        if (taken) continue;
        struct entry_loc slot; int e;
        uint32_t dir;
        if ((e = alloc_cluster(v, &dir)) || (e = zero_cluster(v, dir))) return e;
        uint8_t ent[32];
        memset(v->buf, 0, v->cb);
        dos_entry(ent, (const uint8_t *)".          ", 0x10, dir, 0, v); memcpy(v->buf, ent, 32);
        dos_entry(ent, (const uint8_t *)"..         ", 0x10, 0, 0, v); memcpy(v->buf + 32, ent, 32);
        if ((e = wr(v, cluster_lba(v, dir), v->spc, v->buf))) return e;
        if ((e = free_slot(v, root, &slot))) return e;
        dos_entry(ent, name, 0x10, dir, 0, v);
        if ((e = entry_write(v, &slot, ent))) return e;
        v->found_dir = dir;
        memcpy(v->r->found, "FOUND.", 6);
        v->r->found[6] = (char)name[8]; v->r->found[7] = (char)name[9]; v->r->found[8] = (char)name[10]; v->r->found[9] = 0;
        return 0;
    }
    return -FS_ENOSPC;
}
static int save_chain(struct vol *v, uint32_t head, uint32_t length)
{
    if (!v->fix) return 0;
    int e = 0;
    if (!(v->r->flags & CIUKI_SCANDISK_FREE_LOST) && v->found_next < CHK_MAX) {
        struct entry_loc slot;
        e = found_directory(v);
        if (!e) e = free_slot(v, v->found_dir, &slot);
        if (!e) {
            uint8_t name[11], ent[32];
            chk_name(name, "FILE", v->found_next++, "CHK");
            uint64_t bytes = (uint64_t)length * v->cb;
            dos_entry(ent, name, 0x20, head, bytes > 0xffffffffu ? 0xffffffffu : (uint32_t)bytes, v);
            if ((e = entry_write(v, &slot, ent))) return e;
            v->r->salvaged_files++;
            return 0;
        }
        if (e != -FS_ENOSPC) return e;
    }
    /* Free the chain: FREE_LOST, the CHK limit or no room for FOUND. */
    uint32_t c = head;
    for (uint32_t i = 0; i < length && valid(v, c); i++) {
        uint32_t next; if ((e = get(v, c, &next)) || (e = set(v, c, 0))) return e;
        c = next;
    }
    return 0;
}
static int lost_chains(struct vol *v)
{
    int e;
    for (uint32_t c = 2; c < v->clusters + 2; c++) {
        if (!(c & 1023)) fs_service();
        uint32_t x; if ((e = get(v, c, &x))) return e;
        if (!x || x == bad_value(v) || bit_get(&v->owned, c)) continue;
        if (valid(v, x)) bit_set(&v->linked, x);
    }
    /* Heads first (no lost cluster links to them), then what is left: cycles. */
    for (unsigned pass = 0; pass < 2; pass++)
        for (uint32_t c = 2; c < v->clusters + 2; c++) {
            if (!(c & 1023)) fs_service();
            uint32_t x; if ((e = get(v, c, &x))) return e;
            if (!x || x == bad_value(v) || bit_get(&v->owned, c) || (!pass && bit_get(&v->linked, c))) continue;
            uint32_t at = c, length = 0;
            for (;;) {
                bit_set(&v->owned, at); length++;
                uint32_t next; if ((e = get(v, at, &next))) return e;
                if (eoc(v, next)) break;
                if (!valid(v, next) || next == bad_value(v) || !next || bit_get(&v->owned, next)) {
                    if ((e = set(v, at, mask(v)))) return e;
                    break;
                }
                at = next;
                if (length > v->clusters) break;
            }
            v->r->lost_chains++; v->r->lost_clusters += length;
            if ((e = save_chain(v, c, length))) return e;
        }
    return 0;
}

/* ---- Thorough: the surface -------------------------------------------------- */
static unsigned bad_in(struct vol *v, uint64_t lba, uint32_t count)
{
    unsigned bad = 0;
    for (uint32_t s = 0; s < count; s++) {
        if (rd(v, lba + s, 1, v->buf + s * 512)) { bad++; memset(v->buf + s * 512, 0, 512); }
    }
    return bad;
}
static int find_pred(struct vol *v, uint32_t c, uint32_t *pred)
{
    for (uint32_t x = 2; x < v->clusters + 2; x++) {
        if (!(x & 1023)) fs_service();
        uint32_t n; int e = get(v, x, &n); if (e) return e;
        if (n == c) { *pred = x; return 0; }
    }
    *pred = 0; return 0;
}
/* The entry whose first cluster is c (a relocated head): a second walk. */
static int find_entry(struct vol *v, uint32_t c, struct entry_loc *out)
{
    uint32_t pending = 0, at = v->type == 32 ? v->root_cluster : 0;
    bool fixed = v->type != 32;
    v->queue_head = v->queue_tail = 0;
    for (;;) {
        uint32_t cur = at;
        for (uint32_t k = 0; k <= v->clusters; k++) {
            uint32_t sectors = fixed ? v->root_sectors : v->spc;
            uint64_t base = fixed ? v->root_sector : cluster_lba(v, cur);
            for (uint32_t s = 0; s < sectors; s++) {
                int e = rd(v, base + s, 1, v->buf); if (e) return e;
                for (unsigned off = 0; off < 512; off += 32) {
                    const uint8_t *ent = v->buf + off;
                    if (!ent[0] || ent[0] == 0xe5 || ent[11] == 0x0f || ent[0] == '.') continue;
                    uint32_t first = entry_first(v, ent);
                    if (first == c) { *out = (struct entry_loc){ base + s, (uint16_t)off }; return 0; }
                    if ((ent[11] & 0x10) && valid(v, first)) (void)queue_push(v, first, 0);
                }
            }
            if (fixed) break;
            uint32_t next; int e = get(v, cur, &next); if (e) return e;
            if (eoc(v, next) || !valid(v, next)) break;
            cur = next;
        }
        if (pending >= v->queue_tail) return -FS_ENOENT;
        uint32_t slot = pending++;
        at = v->queue[slot / 512][(slot % 512) * 2]; fixed = false;
        fs_service();
    }
}
static int surface(struct vol *v)
{
    int e;
    uint32_t system = v->data_sector;
    for (uint32_t s = 0; s < system; s += RUN_BYTES / 512) {
        uint32_t n = system - s < RUN_BYTES / 512 ? system - s : RUN_BYTES / 512;
        fs_service();
        if (rd(v, s, n, v->buf)) v->r->unreadable_system += bad_in(v, s, n);
        v->r->surface_sectors += n;
    }
    for (uint32_t c = 2; c < v->clusters + 2; c++) {
        if (!(c & 63)) fs_service();
        uint64_t lba = cluster_lba(v, c);
        v->r->surface_sectors += v->spc;
        if (!rd(v, lba, v->spc, v->buf)) continue;
        unsigned bad = bad_in(v, lba, v->spc);
        if (!bad) continue;
        v->r->bad_sectors += bad;
        uint32_t x; if ((e = get(v, c, &x))) return e;
        if (x == bad_value(v)) continue;
        if (!x) { if ((e = set(v, c, bad_value(v)))) return e; v->r->bad_marked++; continue; }
        if (!v->fix) continue;
        /* Move the readable sectors (zeros stand for the bad ones). */
        uint32_t copy;
        if ((e = alloc_cluster(v, &copy))) { if (e == -FS_ENOSPC) continue; return e; }
        if ((e = wr(v, cluster_lba(v, copy), v->spc, v->buf))) return e;
        if ((e = set(v, copy, x))) return e;
        uint32_t pred; if ((e = find_pred(v, c, &pred))) return e;
        if (pred) { if ((e = set(v, pred, copy))) return e; }
        else {
            struct entry_loc loc; uint8_t ent[32];
            if ((e = find_entry(v, c, &loc))) { if (e != -FS_ENOENT) return e; continue; }
            if ((e = entry_read(v, &loc, ent))) return e;
            entry_set_first(v, ent, copy);
            if ((e = entry_write(v, &loc, ent))) return e;
        }
        if ((e = set(v, c, bad_value(v)))) return e;
        v->r->relocated++;
    }
    return 0;
}

/* ---- census and mirror ------------------------------------------------------ */
static int census(struct vol *v, bool first)
{
    uint32_t free = 0, bad = 0; int e;
    for (uint32_t c = 2; c < v->clusters + 2; c++) {
        if (!(c & 1023)) fs_service();
        uint32_t x; if ((e = get(v, c, &x))) return e;
        if (!x) free++; else if (x == bad_value(v)) bad++;
    }
    v->r->free_clusters = free; v->r->bad_clusters = bad;
    if (first && v->type != 12) {
        uint32_t x; if ((e = get(v, 1, &x))) return e;
        uint32_t clean = v->type == 16 ? 0x8000 : 0x08000000, hard = clean >> 1;
        if (!(x & clean)) v->r->flags |= CIUKI_SCANDISK_DIRTY;
        if (!(x & hard)) v->r->flags |= CIUKI_SCANDISK_HARD_ERROR;
    }
    return 0;
}
/* Count (first pass) or rewrite (final pass) other-copy sectors that differ. */
static int mirror(struct vol *v, bool rewrite)
{
    if (v->fats < 2 || !v->mirror) return 0;
    int e = win_flush(v); if (e) return e;
    v->win_valid = false;
    uint8_t *other = v->buf + RUN_BYTES / 2;
    for (uint32_t s = 0; s < v->fat_sectors; s += 32) {
        uint32_t n = v->fat_sectors - s < 32 ? v->fat_sectors - s : 32;
        fs_service();
        bool missing, bad_active[32] = { false };
        if (rd(v, v->reserved + (uint64_t)v->active * v->fat_sectors + s, n, v->buf))
            for (uint32_t i = 0; i < n; i++) {
                if ((e = fat_sector(v, v->active, s + i, v->buf + i * 512, &missing))) return e;
                bad_active[i] = missing;
            }
        for (uint32_t f = 0; f < v->fats; f++) {
            uint64_t base = v->reserved + (uint64_t)f * v->fat_sectors + s;
            if (f == v->active) {
                /* An unreadable active sector is rewritten from the mirror. */
                for (uint32_t i = 0; i < n; i++) if (bad_active[i]) {
                    if (!rewrite) v->r->fat_mismatches++;
                    else if ((e = wr(v, base + i, 1, v->buf + i * 512))) return e;
                }
                continue;
            }
            bool whole = !rd(v, base, n, other);
            for (uint32_t i = 0; i < n; i++) {
                bool unreadable = !whole && rd(v, base + i, 1, other + i * 512);
                if (!unreadable && !memcmp(v->buf + i * 512, other + i * 512, 512)) continue;
                if (!rewrite) v->r->fat_mismatches++;
                else if ((e = wr(v, base + i, 1, v->buf + i * 512))) return e;
            }
        }
    }
    return 0;
}

static int geometry(struct vol *v)
{
    int e = rd(v, 0, 1, v->buf); if (e) return e;
    const uint8_t *b = v->buf;
    if (fs_rd16(b + 510) != 0xaa55 || fs_rd16(b + 11) != 512 || !b[13] || (b[13] & (b[13] - 1)) ||
        !fs_rd16(b + 14) || !b[16] || b[16] > 4) return -FS_EINVAL;
    v->spc = b[13]; v->cb = v->spc * 512; v->reserved = fs_rd16(b + 14); v->fats = b[16];
    v->root_entries = fs_rd16(b + 17);
    v->total = fs_rd16(b + 19) ? fs_rd16(b + 19) : fs_rd32(b + 32);
    v->fat_sectors = fs_rd16(b + 22) ? fs_rd16(b + 22) : fs_rd32(b + 36);
    v->root_sectors = (v->root_entries * 32 + 511) / 512;
    if (!v->fat_sectors || !v->total || v->total > v->dev->capacity) return -FS_EINVAL;
    uint64_t meta = (uint64_t)v->reserved + (uint64_t)v->fats * v->fat_sectors + v->root_sectors;
    if (meta >= v->total) return -FS_EINVAL;
    v->root_sector = v->reserved + v->fats * v->fat_sectors;
    v->data_sector = v->root_sector + v->root_sectors;
    v->clusters = (uint32_t)((v->total - meta) / v->spc);
    v->type = v->clusters < 4085 ? 12 : v->clusters < 65525 ? 16 : 32;
    if ((v->type == 32) != !fs_rd16(b + 22) || (v->type == 32 && v->root_entries)) return -FS_EINVAL;
    v->mirror = true; v->active = 0;
    if (v->type == 32) {
        uint16_t ext = fs_rd16(b + 40);
        if (ext & 0x80) { v->mirror = false; v->active = ext & 0x0f; if (v->active >= v->fats) return -FS_EINVAL; }
        v->root_cluster = fs_rd32(b + 44);
        if (!valid(v, v->root_cluster)) return -FS_EINVAL;
    }
    uint64_t fat_bytes = (uint64_t)v->fat_sectors * 512, needed = (uint64_t)fat_offset(v, v->clusters + 1) + (v->type == 12 ? 2 : v->type / 8);
    if (fat_bytes < needed) return -FS_EINVAL;
    v->r->fat_type = v->type; v->r->cluster_bytes = v->cb; v->r->clusters = v->clusters;
    return 0;
}

static int fsinfo_unknown(struct vol *v)
{
    if (v->type != 32 || !v->fix || !v->changed) return 0;
    int e = rd(v, 0, 1, v->buf); if (e) return e;
    uint32_t sector = fs_rd16(v->buf + 48);
    if (!sector || sector >= v->reserved) return 0;
    if ((e = rd(v, sector, 1, v->buf))) return e;
    if (fs_rd32(v->buf) != 0x41615252u || fs_rd32(v->buf + 484) != 0x61417272u) return 0;
    fs_wr32(v->buf + 488, 0xffffffffu); fs_wr32(v->buf + 492, 0xffffffffu);
    return wr(v, sector, 1, v->buf);
}

int scandisk_run(struct blkdev *dev, struct ciuki_scandisk *r)
{
    uint32_t began = fs_now_ms();
    uint32_t in = r->flags & (CIUKI_SCANDISK_FIX | CIUKI_SCANDISK_THOROUGH | CIUKI_SCANDISK_FREE_LOST | CIUKI_SCANDISK_NO_COPY);
    uint32_t drive = r->drive, keep = r->flags & CIUKI_SCANDISK_AT_START;
    memset(r, 0, sizeof(*r));
    r->version = CIUKI_SCANDISK_VERSION; r->size = sizeof(*r); r->drive = drive;
    r->flags = in | keep | CIUKI_SCANDISK_RAN;
    struct vol *v = fs_contig_alloc(sizeof(*v));
    if (!v) { r->result = -FS_ENOMEM; return r->result; }
    v->dev = dev; v->r = r; v->fix = in & CIUKI_SCANDISK_FIX; v->thorough = in & CIUKI_SCANDISK_THOROUGH;
    v->win = fs_contig_alloc(WINDOW_SECTORS * 512);
    v->buf = fs_contig_alloc(RUN_BYTES);
    int e = !v->win || !v->buf ? -FS_ENOMEM : !dev || dev->sector_size != 512 ? -FS_EINVAL : 0;
    if (!e && v->fix && !blkdev_durable(dev)) e = -FS_EROFS;
    if (!e) e = geometry(v);
    if (!e) e = bits_init(&v->owned, v->clusters + 2);
    if (!e) e = bits_init(&v->linked, v->clusters + 2);
    if (!e) e = census(v, true);
    if (!e) e = mirror(v, false);
    if (!e) e = scan_tree(v);
    if (!e) e = copy_crosslinks(v);
    if (!e) e = lost_chains(v);
    if (!e && v->thorough) e = surface(v);
    if (!e) e = win_flush(v);
    if (!e && v->fix) e = mirror(v, true);
    if (!e) e = fsinfo_unknown(v);
    if (!e && v->fix && v->changed && dev->flush && dev->write_cache_state != BLKDEV_CACHE_DISABLED) e = dev->flush(dev);
    if (!e) { v->win_valid = false; e = census(v, false); }
    if (!e && v->error) e = v->error;
    uint32_t errors = r->lost_chains + r->crosslinked_files + r->invalid_chains + r->size_mismatches +
        r->invalid_entries + r->fat_mismatches + r->bad_marked + r->relocated + r->unreadable_system;
    if (!v->fix && r->bad_sectors) errors++;
    if (errors) r->flags |= CIUKI_SCANDISK_ERRORS;
    bool left = r->crosslinks_fixed < r->crosslinked_files || r->unreadable_system ||
        (r->bad_sectors && r->bad_marked + r->relocated == 0);
    if (!e && v->fix && errors && !left) r->flags |= CIUKI_SCANDISK_FIXED;
    r->result = e;
    r->elapsed_ms = fs_now_ms() - began;
    bits_free(&v->owned); bits_free(&v->linked);
    for (uint32_t i = 0; i < v->queue_pages; i++) fs_page_free(v->queue[i]);
    if (v->win) fs_contig_free(v->win, WINDOW_SECTORS * 512);
    if (v->buf) fs_contig_free(v->buf, RUN_BYTES);
    fs_contig_free(v, sizeof(*v));
    if (klog) klog("[scandisk] drive=%c result=%d fix=%u thorough=%u fat=%u dirty=%u lost=%u/%u chk=%u crosslinked=%u/%u invalid=%u sizes=%u entries=%u mirror=%u bad=%u/%u/%u ms=%u",
        'A' + (char)(drive < 26 ? drive : 25), e, !!(in & CIUKI_SCANDISK_FIX), !!(in & CIUKI_SCANDISK_THOROUGH), r->fat_type,
        !!(r->flags & CIUKI_SCANDISK_DIRTY), r->lost_chains, r->lost_clusters, r->salvaged_files,
        r->crosslinks_fixed, r->crosslinked_files, r->invalid_chains, r->size_mismatches, r->invalid_entries,
        r->fat_mismatches, r->bad_sectors, r->bad_marked, r->relocated, r->elapsed_ms);
    return e;
}
