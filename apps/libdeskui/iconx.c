/* SPDX-License-Identifier: MIT
 * iconx: bounded icon extraction core (iconx.h). Freestanding and
 * integer-only; it is compiled into libdeskui and into the Win32 USER
 * facade (scripts/win32/user32.py), so it uses no libc function at all. */
#include "iconx.h"

static void zero(void *p, uint32_t n) { uint8_t *q = p; while (n--) *q++ = 0; }
static void move(void *d, const void *s, uint32_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; }
static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

void iconx_init(struct iconx *x, iconx_read_fn read, void *ctx, uint32_t size)
{
    x->read = read; x->ctx = ctx; x->size = size; x->base = 0; x->limit = size;
    x->budget = ICONX_READ_BUDGET; x->loaded = x->calls = 0; x->clock = 0;
    for (unsigned k = 0; k < ICONX_CACHE; k++) { x->cache[k].at = 0; x->cache[k].len = 0; x->cache[k].age = 0; }
    x->kind = -1; x->group_id = 0;
    zero(&x->chosen, sizeof(x->chosen));
}

int iconx_memory_read(void *ctx, uint32_t offset, void *buffer, uint32_t bytes)
{
    const struct iconx_memory *m = ctx;
    if (!m || offset > m->size || bytes > m->size - offset) return -1;
    move(buffer, m->bytes + offset, bytes);
    return 0;
}

/* [off, off + n) of the current window, through the read cache. */
static int get(struct iconx *x, uint32_t off, void *out, uint32_t n)
{
    if (!x->read || off > x->limit || n > x->limit - off) return -1;
    if (!n) return 0;
    uint32_t at = x->base + off;
    if (n > ICONX_CACHE_BYTES) {
        if (n > x->budget) return -1;
        x->budget -= n; x->loaded += n; x->calls++;
        return x->read(x->ctx, at, out, n) ? -1 : 0;
    }
    unsigned victim = 0;
    for (unsigned k = 0; k < ICONX_CACHE; k++) {
        if (x->cache[k].len >= n && at >= x->cache[k].at && at - x->cache[k].at <= x->cache[k].len - n) {
            move(out, x->cache[k].bytes + (at - x->cache[k].at), n);
            x->cache[k].age = ++x->clock;
            return 0;
        }
        if (!x->cache[k].len || x->cache[k].age < x->cache[victim].age) victim = k;
    }
    /* at + n <= base + limit <= size, so the line holds the whole request. */
    uint32_t len = x->size - at < ICONX_CACHE_BYTES ? x->size - at : ICONX_CACHE_BYTES;
    if (len < n || len > x->budget) return -1;
    x->budget -= len; x->loaded += len; x->calls++;
    if (x->read(x->ctx, at, x->cache[victim].bytes, len)) { x->cache[victim].len = 0; return -1; }
    x->cache[victim].at = at; x->cache[victim].len = len; x->cache[victim].age = ++x->clock;
    move(out, x->cache[victim].bytes, n);
    return 0;
}
static int get16(struct iconx *x, uint32_t off, uint32_t *v)
{ uint8_t b[2]; if (get(x, off, b, 2)) return -1; *v = le16(b); return 0; }
static int get32(struct iconx *x, uint32_t off, uint32_t *v)
{ uint8_t b[4]; if (get(x, off, b, 4)) return -1; *v = le32(b); return 0; }

int iconx_kind(struct iconx *x)
{
    if (x->kind >= 0) return x->kind;
    uint8_t h[64];
    uint32_t n = x->limit < sizeof(h) ? x->limit : (uint32_t)sizeof(h);
    x->kind = ICONX_UNKNOWN;
    if (n < 6 || get(x, 0, h, n)) return x->kind;
    uint32_t type = le16(h + 2), count = le16(h + 4);
    if (!le16(h) && (type == 1 || type == 2) && count && count <= ICONX_GROUP_MAX && 6 + 16 * count <= x->limit)
        x->kind = type == 1 ? ICONX_ICO : ICONX_CUR;
    else if (n >= 12 && le32(h) == FOURCC('R', 'I', 'F', 'F') && le32(h + 8) == FOURCC('A', 'C', 'O', 'N'))
        x->kind = ICONX_ANI;
    else if (n >= 26 && h[0] == 'B' && h[1] == 'M')
        x->kind = ICONX_BMP;
    else if ((h[0] == 'M' && h[1] == 'Z') || (h[0] == 'Z' && h[1] == 'M')) {
        x->kind = ICONX_DOS;
        uint32_t lfanew = n >= 64 ? le32(h + 60) : 0;
        uint8_t s[4];
        if (lfanew >= 4 && lfanew <= x->limit - 4 && !get(x, lfanew, s, 4)) {
            if (s[0] == 'P' && s[1] == 'E' && !s[2] && !s[3]) x->kind = ICONX_PE;
            else if (s[0] == 'N' && s[1] == 'E') x->kind = ICONX_NE;
        }
    }
    return x->kind;
}

/* ---- DIB images ---------------------------------------------------------- */
struct dib {
    uint32_t bits, mask;
    unsigned w, h, bpp, pitch, mask_pitch;
    int topdown, has_mask, alpha, icon;
};

static int dib_open(struct iconx *x, uint32_t at, uint32_t bytes, int icon, uint32_t bits_at, struct dib *d)
{
    if (at > x->limit) return ICONX_EFORMAT;
    if (bytes > x->limit - at) bytes = x->limit - at;
    uint8_t h[40];
    if (bytes < 12 || get(x, at, h, 12)) return ICONX_EFORMAT;
    if (h[0] == 0x89 && h[1] == 'P' && h[2] == 'N' && h[3] == 'G') return ICONX_EFORMAT;   /* Vista PNG image */
    uint32_t hs = le32(h), colors, palsize = 4, planes;
    int32_t width, height;
    zero(d, sizeof(*d));
    d->icon = icon;
    if (hs == 12) {
        width = (int32_t)le16(h + 4); height = (int32_t)le16(h + 6); planes = le16(h + 8); d->bpp = le16(h + 10);
        if (d->bpp > 8 && d->bpp != 24) return ICONX_EFORMAT;
        colors = d->bpp <= 8 ? 1u << d->bpp : 0; palsize = 3;
    } else if (hs >= 40 && hs <= 124) {
        if (bytes < 40 || get(x, at, h, 40)) return ICONX_EFORMAT;
        width = (int32_t)le32(h + 4); height = (int32_t)le32(h + 8); planes = le16(h + 12); d->bpp = le16(h + 14);
        uint32_t compression = le32(h + 16), used = le32(h + 32);
        if (compression) return ICONX_EFORMAT;            /* BI_RGB only */
        if (used > 256) return ICONX_EFORMAT;
        colors = d->bpp <= 8 ? (used ? used : 1u << d->bpp) : used;
    } else return ICONX_EFORMAT;
    if (planes > 1 || width <= 0 || width > (int32_t)ICONX_SIZE_MAX) return ICONX_EFORMAT;
    if (d->bpp != 1 && d->bpp != 4 && d->bpp != 8 && d->bpp != 16 && d->bpp != 24 && d->bpp != 32) return ICONX_EFORMAT;
    if (icon) {
        if (height < 2) return ICONX_EFORMAT;
        height /= 2;
    } else if (height < 0) {
        if (height < -(int32_t)ICONX_SIZE_MAX) return ICONX_EFORMAT;
        height = -height; d->topdown = 1;
    }
    if (!height || height > (int32_t)ICONX_SIZE_MAX) return ICONX_EFORMAT;
    d->w = (unsigned)width; d->h = (unsigned)height;
    d->pitch = ((d->w * d->bpp + 31) / 32) * 4;
    d->mask_pitch = ((d->w + 31) / 32) * 4;
    uint32_t table = at + hs, end = at + bytes;
    if (hs > bytes || colors * palsize > bytes - hs) return ICONX_EFORMAT;
    d->bits = bits_at ? bits_at : table + colors * palsize;
    if (d->bits < table || d->bits > end || (uint32_t)d->pitch * d->h > end - d->bits) return ICONX_EFORMAT;
    if (icon) {
        d->mask = d->bits + d->pitch * d->h;
        d->has_mask = (uint32_t)d->mask_pitch * d->h <= end - d->mask;
        if (!d->has_mask && d->bpp != 32) return ICONX_EFORMAT;
    }
    unsigned palette = colors < 256 ? colors : 256;
    for (unsigned k = 0; k < 256; k++) x->palette[k] = 0;
    for (unsigned k = 0; k < palette; k++) {
        uint8_t q[4];
        if (get(x, table + k * palsize, q, palsize)) return ICONX_EFORMAT;
        x->palette[k] = (uint32_t)q[2] << 16 | (uint32_t)q[1] << 8 | q[0];
    }
    if (d->bpp == 32)   /* XP rule: any alpha byte means a real alpha channel */
        for (unsigned y = 0; y < d->h && !d->alpha; y++) {
            if (get(x, d->bits + y * d->pitch, x->raw, d->pitch)) return ICONX_EFORMAT;
            for (unsigned k = 0; k < d->w; k++) if (x->raw[k * 4 + 3]) { d->alpha = 1; break; }
        }
    return ICONX_OK;
}

/* Source row sy (0 = top) as straight-alpha ARGB in x->row. */
static int dib_row(struct iconx *x, const struct dib *d, unsigned sy)
{
    unsigned file_row = d->topdown ? sy : d->h - 1 - sy;
    if (get(x, d->bits + file_row * d->pitch, x->raw, d->pitch)) return -1;
    if (d->has_mask && get(x, d->mask + file_row * d->mask_pitch, x->mask, d->mask_pitch)) return -1;
    const uint8_t *r = x->raw;
    for (unsigned sx = 0; sx < d->w; sx++) {
        uint32_t c, a = 255;
        switch (d->bpp) {
        case 1: c = x->palette[(r[sx >> 3] >> (7 - (sx & 7))) & 1]; break;
        case 4: c = x->palette[(r[sx >> 1] >> (sx & 1 ? 0 : 4)) & 15]; break;
        case 8: c = x->palette[r[sx]]; break;
        case 16: {
            uint32_t v = le16(r + sx * 2), r5 = v >> 10 & 31, g5 = v >> 5 & 31, b5 = v & 31;
            c = (r5 << 3 | r5 >> 2) << 16 | (g5 << 3 | g5 >> 2) << 8 | (b5 << 3 | b5 >> 2);
            break;
        }
        case 24: c = (uint32_t)r[sx * 3 + 2] << 16 | (uint32_t)r[sx * 3 + 1] << 8 | r[sx * 3]; break;
        default: c = le32(r + sx * 4) & 0xffffffu; if (d->alpha) a = r[sx * 4 + 3]; break;
        }
        if (!d->alpha && d->has_mask && (x->mask[sx >> 3] >> (7 - (sx & 7)) & 1)) {
            if (!c) a = 0;               /* transparent */
            else c ^= 0xffffffu;         /* screen inversion, shown over white */
        }
        x->row[sx] = a ? a << 24 | c : 0;
    }
    return 0;
}

/* Integer area filter, w x h -> size x size, streamed bottom-up so the
 * stored rows are read in ascending file order. */
static int dib_scale(struct iconx *x, const struct dib *d, unsigned size, uint32_t *out)
{
    const unsigned w = d->w, h = d->h;
    const uint32_t total = (uint32_t)w * h;
    int decoded = -1;
    for (unsigned dy = size; dy-- > 0;) {
        for (unsigned dx = 0; dx < size; dx++) x->sums[dx][0] = x->sums[dx][1] = x->sums[dx][2] = x->sums[dx][3] = 0;
        uint32_t ylo = dy * h, yhi = ylo + h;
        for (unsigned sy = (yhi - 1) / size + 1; sy-- > ylo / size;) {
            uint32_t top = sy * size > ylo ? sy * size : ylo, bottom = (sy + 1) * size < yhi ? (sy + 1) * size : yhi;
            uint32_t wy = bottom - top;
            if ((int)sy != decoded) { if (dib_row(x, d, sy)) return ICONX_EFORMAT; decoded = (int)sy; }
            for (unsigned dx = 0; dx < size; dx++) {
                uint32_t xlo = dx * w, xhi = xlo + w;
                for (unsigned sx = xlo / size; sx <= (xhi - 1) / size; sx++) {
                    uint32_t left = sx * size > xlo ? sx * size : xlo, right = (sx + 1) * size < xhi ? (sx + 1) * size : xhi;
                    uint32_t p = x->row[sx], wa = (right - left) * wy * (p >> 24);
                    x->sums[dx][0] += wa; x->sums[dx][1] += wa * (p >> 16 & 255);
                    x->sums[dx][2] += wa * (p >> 8 & 255); x->sums[dx][3] += wa * (p & 255);
                }
            }
        }
        for (unsigned dx = 0; dx < size; dx++) {
            uint32_t sa = x->sums[dx][0], a = (sa + total / 2) / total;
            out[dy * size + dx] = a && sa ? a << 24 | ((x->sums[dx][1] + sa / 2) / sa) << 16 |
                                  ((x->sums[dx][2] + sa / 2) / sa) << 8 | (x->sums[dx][3] + sa / 2) / sa : 0;
        }
    }
    return ICONX_OK;
}

static int decode_at(struct iconx *x, uint32_t at, uint32_t bytes, int icon, uint32_t bits_at, unsigned size, uint32_t *out)
{
    struct dib d;
    int e = dib_open(x, at, bytes, icon, bits_at, &d);
    if (e) return e;
    e = dib_scale(x, &d, size, out);
    if (!e) { x->chosen.width = d.w; x->chosen.height = d.h; x->chosen.depth = d.bpp; }
    return e;
}

int iconx_decode(struct iconx *x, uint32_t offset, uint32_t bytes, unsigned size, uint32_t *argb)
{
    if (!argb || !size || size > ICONX_SIZE_MAX) return ICONX_EARG;
    return decode_at(x, offset, bytes, 1, 0, size, argb);
}

/* ---- icon directories ----------------------------------------------------
 * Rank keys are unique (the entry index is the low part): pass t returns the
 * smallest key above the previous one, so ICONX_TRIES passes visit the best
 * candidates in order without storing the directory. */
static uint32_t entry_key(const uint8_t *e, unsigned index, int cursor, unsigned size)
{
    unsigned w = e[0] ? e[0] : 256, h = e[1] ? e[1] : 256, colors = e[2], depth = cursor ? 0 : le16(e + 6);
    if (!depth) depth = colors == 2 ? 1 : colors == 4 ? 2 : colors == 16 ? 4 : 8;
    if (depth > 32) depth = 32;
    unsigned dist = (w > size ? w - size : size - w) + (h > size ? h - size : size - h);
    return (uint32_t)dist << 16 | (uint32_t)(w < size || h < size) << 15 | (uint32_t)(32 - depth) << 9 | index;
}
/* The entry with the smallest key above `after` in a directory read through
 * get(); -1 when none is left. */
static int next_best(struct iconx *x, uint32_t dir, unsigned count, unsigned stride, int cursor, unsigned size, uint32_t *after)
{
    uint32_t best = 0xffffffffu; int found = -1;
    for (unsigned k = 0; k < count; k++) {
        uint8_t e[16];
        if (get(x, dir + 6 + k * stride, e, stride)) return -1;
        uint32_t key = entry_key(e, k, cursor, size);
        if ((*after == 0xffffffffu || key > *after) && (found < 0 || key < best)) { best = key; found = (int)k; }
    }
    if (found >= 0) *after = best;
    return found;
}

int iconx_rank(const uint8_t *dir, uint32_t bytes, unsigned entry, unsigned size, unsigned *order, unsigned max)
{
    if (!dir || (entry != 14 && entry != 16) || !size || size > ICONX_SIZE_MAX || bytes < 6) return ICONX_EARG;
    unsigned count = le16(dir + 4);
    if (le16(dir) || (le16(dir + 2) != 1 && le16(dir + 2) != 2) || !count) return ICONX_EFORMAT;
    if (count > ICONX_GROUP_MAX) return ICONX_ELIMIT;
    if (6 + entry * count > bytes) return ICONX_EFORMAT;
    uint32_t after = 0xffffffffu; unsigned n = 0;
    while (n < max) {
        uint32_t best = 0xffffffffu; int found = -1;
        for (unsigned k = 0; k < count; k++) {
            uint32_t key = entry_key(dir + 6 + k * entry, k, le16(dir + 2) == 2, size);
            if ((after == 0xffffffffu || key > after) && (found < 0 || key < best)) { best = key; found = (int)k; }
        }
        if (found < 0) break;
        order[n++] = (unsigned)found; after = best;
    }
    return (int)n;
}

/* An ICO/CUR file at the start of the current window. */
static int file_extract(struct iconx *x, unsigned size, uint32_t *out)
{
    uint32_t type, count;
    if (get16(x, 2, &type) || get16(x, 4, &count)) return ICONX_EIO;
    if ((type != 1 && type != 2) || !count || count > ICONX_GROUP_MAX || 6 + 16 * count > x->limit) return ICONX_EFORMAT;
    uint32_t after = 0xffffffffu; int e = ICONX_EFORMAT;
    for (unsigned t = 0; t < ICONX_TRIES; t++) {
        int k = next_best(x, 0, count, 16, type == 2, size, &after);
        if (k < 0) break;
        uint8_t entry[16];
        if (get(x, 6 + (uint32_t)k * 16, entry, 16)) return ICONX_EIO;
        uint32_t bytes = le32(entry + 8), at = le32(entry + 12);
        if (at < 6 + 16 * count) continue;
        e = decode_at(x, at, bytes, 1, 0, size, out);
        if (!e) { x->chosen.index = (unsigned)k; x->chosen.id = 0; return ICONX_OK; }
    }
    return e;
}

/* ---- PE resources ---------------------------------------------------------- */
struct pe { uint32_t sections, count, root, span; };

static int pe_map(struct iconx *x, const struct pe *p, uint32_t rva, uint32_t *off, uint32_t *avail)
{
    for (uint32_t k = 0; k < p->count; k++) {
        uint8_t s[40];
        if (get(x, p->sections + k * 40, s, 40)) return -1;
        uint32_t va = le32(s + 12), raw = le32(s + 16), at = le32(s + 20);
        if (rva < va || rva - va >= raw) continue;
        uint32_t o = rva - va;
        if (at > x->limit || o > x->limit - at) return -1;
        *off = at + o;
        *avail = raw - o;
        if (*avail > x->limit - *off) *avail = x->limit - *off;
        return 0;
    }
    return -1;
}
static int pe_open(struct iconx *x, struct pe *p)
{
    uint32_t lfanew, optional, magic, number, rva, size;
    uint8_t coff[24];
    if (get32(x, 60, &lfanew) || get(x, lfanew, coff, 24)) return ICONX_EFORMAT;
    p->count = le16(coff + 6);
    uint32_t optsize = le16(coff + 20);
    optional = lfanew + 24;
    if (!p->count || p->count > 96 || get16(x, optional, &magic)) return ICONX_EFORMAT;
    uint32_t count_at = magic == 0x10b ? 92 : magic == 0x20b ? 108 : 0;
    if (!count_at || optsize < count_at + 4 + 3 * 8 || get32(x, optional + count_at, &number)) return ICONX_EFORMAT;
    if (number < 3) return ICONX_ENONE;
    if (get32(x, optional + count_at + 4 + 16, &rva) || get32(x, optional + count_at + 4 + 20, &size)) return ICONX_EFORMAT;
    if (!rva || !size) return ICONX_ENONE;
    p->sections = optional + optsize;
    if (p->sections > x->limit) return ICONX_EFORMAT;
    if (pe_map(x, p, rva, &p->root, &p->span)) return ICONX_EFORMAT;
    return ICONX_OK;
}
/* Directory at rel: first entry offset and counts. */
static int pe_dir(struct iconx *x, const struct pe *p, uint32_t rel, uint32_t *first, unsigned *named, unsigned *ids)
{
    uint8_t h[16];
    if (rel > p->span || p->span - rel < 16 || get(x, p->root + rel, h, 16)) return ICONX_EFORMAT;
    *named = le16(h + 12); *ids = le16(h + 14);
    if (*named + *ids > ICONX_DIR_MAX) return ICONX_ELIMIT;
    if ((uint32_t)(*named + *ids) * 8 > p->span - rel - 16) return ICONX_EFORMAT;
    *first = rel + 16;
    return ICONX_OK;
}
static int pe_entry(struct iconx *x, const struct pe *p, uint32_t first, unsigned k, uint32_t *name, uint32_t *target)
{
    uint8_t e[8];
    if (get(x, p->root + first + k * 8, e, 8)) return ICONX_EIO;
    *name = le32(e); *target = le32(e + 4);
    return ICONX_OK;
}
static int pe_find(struct iconx *x, const struct pe *p, uint32_t rel, uint32_t id, uint32_t *target)
{
    uint32_t first, name; unsigned named, ids;
    int e = pe_dir(x, p, rel, &first, &named, &ids);
    if (e) return e;
    for (unsigned k = named; k < named + ids; k++) {
        if ((e = pe_entry(x, p, first, k, &name, target))) return e;
        if (name == id) return ICONX_OK;
    }
    return ICONX_ENONE;
}
/* Down the language level (its first entry, as Wine's find_entry_default)
 * to a data entry: file offset and size of the resource. */
static int pe_leaf(struct iconx *x, const struct pe *p, uint32_t target, uint32_t *off, uint32_t *bytes)
{
    for (unsigned depth = 0; target & 0x80000000u; depth++) {
        uint32_t first, name; unsigned named, ids;
        if (depth == 2) return ICONX_EFORMAT;
        int e = pe_dir(x, p, target & 0x7fffffffu, &first, &named, &ids);
        if (e) return e;
        if (!named && !ids) return ICONX_ENONE;
        if ((e = pe_entry(x, p, first, 0, &name, &target))) return e;
    }
    uint8_t d[8];
    uint32_t avail;
    if (target > p->span || p->span - target < 16 || get(x, p->root + target, d, 8)) return ICONX_EFORMAT;
    if (pe_map(x, p, le32(d), off, &avail) || le32(d + 4) > avail) return ICONX_EFORMAT;
    *bytes = le32(d + 4);
    return ICONX_OK;
}
static int pe_groups(struct iconx *x, const struct pe *p, uint32_t *first, unsigned *named, unsigned *ids)
{
    uint32_t target;
    int e = pe_find(x, p, 0, 14, &target);
    if (e) return e;
    if (!(target & 0x80000000u)) return ICONX_EFORMAT;
    return pe_dir(x, p, target & 0x7fffffffu, first, named, ids);
}

/* An icon group directory at [at, at + bytes) whose images come from
 * image(id): shared by PE and NE. */
typedef int (*image_fn)(struct iconx *, const void *, uint32_t id, uint32_t *off, uint32_t *bytes);
static int group_extract(struct iconx *x, uint32_t at, uint32_t bytes, image_fn image, const void *ctx, unsigned size, uint32_t *out)
{
    uint32_t reserved, type, count;
    if (bytes < 6 || get16(x, at, &reserved) || get16(x, at + 2, &type) || get16(x, at + 4, &count)) return ICONX_EFORMAT;
    if (reserved || type != 1 || !count) return ICONX_EFORMAT;
    if (count > ICONX_GROUP_MAX) return ICONX_ELIMIT;
    if (6 + 14 * count > bytes) return ICONX_EFORMAT;
    uint32_t after = 0xffffffffu; int e = ICONX_EFORMAT;
    for (unsigned t = 0; t < ICONX_TRIES; t++) {
        int k = next_best(x, at, count, 14, 0, size, &after);
        if (k < 0) break;
        uint32_t id, off, len;
        if (get16(x, at + 6 + (uint32_t)k * 14 + 12, &id)) return ICONX_EIO;
        if (image(x, ctx, id, &off, &len)) continue;
        e = decode_at(x, off, len, 1, 0, size, out);
        if (!e) { x->chosen.index = (unsigned)k; x->chosen.id = id; return ICONX_OK; }
    }
    return e;
}
static int pe_image(struct iconx *x, const void *ctx, uint32_t id, uint32_t *off, uint32_t *bytes)
{
    const struct pe *p = ctx;
    uint32_t icons, target;
    if (pe_find(x, p, 0, 3, &icons) || !(icons & 0x80000000u)) return -1;
    if (pe_find(x, p, icons & 0x7fffffffu, id, &target)) return -1;
    return pe_leaf(x, p, target, off, bytes) ? -1 : 0;
}
static int pe_extract(struct iconx *x, int index, unsigned size, uint32_t *out)
{
    struct pe p;
    uint32_t first, name, target, off, bytes; unsigned named, ids;
    int e = pe_open(x, &p);
    if (!e) e = pe_groups(x, &p, &first, &named, &ids);
    if (e) return e;
    if (index >= 0) {
        if ((unsigned)index >= named + ids) return ICONX_EINDEX;
        if ((e = pe_entry(x, &p, first, (unsigned)index, &name, &target))) return e;
    } else {
        uint32_t id = index == -2147483647 - 1 ? 0 : (uint32_t)-index;
        unsigned k = named;
        for (; k < named + ids; k++) {
            if ((e = pe_entry(x, &p, first, k, &name, &target))) return e;
            if (name == id) break;
        }
        if (k == named + ids) return ICONX_EINDEX;
    }
    x->group_id = name & 0x80000000u ? 0 : name;
    if ((e = pe_leaf(x, &p, target, &off, &bytes))) return e;
    return group_extract(x, off, bytes, pe_image, &p, size, out);
}

/* ---- NE resources ------------------------------------------------------------ */
struct ne { uint32_t table, end, shift; };

static int ne_open(struct iconx *x, struct ne *n)
{
    uint32_t lfanew, rsrc, res;
    if (get32(x, 60, &lfanew) || lfanew > 0xffff0000u || get16(x, lfanew + 0x24, &rsrc) || get16(x, lfanew + 0x26, &res))
        return ICONX_EFORMAT;
    n->table = lfanew + rsrc; n->end = lfanew + res;
    if (res <= rsrc || n->end - n->table <= 8) return ICONX_ENONE;   /* Wine: no TYPEINFO fits, no resources */
    if (n->end > x->limit || get16(x, n->table, &n->shift) || n->shift > 20) return ICONX_EFORMAT;
    return ICONX_OK;
}
/* The NAMEINFO array of a resource type: offset of its first record and count. */
static int ne_type(struct iconx *x, const struct ne *n, uint32_t type, uint32_t *names, uint32_t *count)
{
    uint32_t at = n->table + 2, total = 0;
    for (unsigned t = 0; t < 256; t++) {
        uint32_t id, c;
        if (at > n->end - 2 || get16(x, at, &id)) return ICONX_EFORMAT;
        if (!id) return ICONX_ENONE;
        if (at > n->end - 8 || get16(x, at + 2, &c)) return ICONX_EFORMAT;
        total += c;
        if (total > ICONX_DIR_MAX) return ICONX_ELIMIT;
        if (c * 12 > n->end - at - 8) return ICONX_EFORMAT;
        if (id == type) { *names = at + 8; *count = c; return ICONX_OK; }
        at += 8 + c * 12;
    }
    return ICONX_ELIMIT;
}
static int ne_data(struct iconx *x, const struct ne *n, uint32_t name, uint32_t *off, uint32_t *bytes)
{
    uint32_t o, l;
    if (get16(x, name, &o) || get16(x, name + 2, &l)) return -1;
    if (n->shift > 16 && ((o >> (32 - n->shift)) || (l >> (32 - n->shift)))) return -1;
    o <<= n->shift; l <<= n->shift;
    if (o >= x->limit) return -1;
    if (l > x->limit - o) l = x->limit - o;   /* the last block may be padded past the end */
    *off = o; *bytes = l;
    return 0;
}
static int ne_image(struct iconx *x, const void *ctx, uint32_t id, uint32_t *off, uint32_t *bytes)
{
    const struct ne *n = ctx;
    uint32_t names, count;
    if (ne_type(x, n, 0x8003, &names, &count)) return -1;
    for (uint32_t k = 0; k < count; k++) {
        uint32_t rid;
        if (get16(x, names + k * 12 + 6, &rid)) return -1;
        if (rid == (id | 0x8000u)) return ne_data(x, n, names + k * 12, off, bytes);
    }
    return -1;
}
static int ne_extract(struct iconx *x, int index, unsigned size, uint32_t *out)
{
    struct ne n;
    uint32_t names, count, k, rid = 0, off, bytes;
    int e = ne_open(x, &n);
    if (!e) e = ne_type(x, &n, 0x800e, &names, &count);
    if (e) return e;
    if (index >= 0) {
        if ((uint32_t)index >= count) return ICONX_EINDEX;
        k = (uint32_t)index;
        if (get16(x, names + k * 12 + 6, &rid)) return ICONX_EIO;
    } else {
        uint32_t id = index < -0x7fff ? 0 : (uint32_t)-index | 0x8000u;
        for (k = 0; k < count; k++) {
            if (get16(x, names + k * 12 + 6, &rid)) return ICONX_EIO;
            if (rid == id) break;
        }
        if (k == count) return ICONX_EINDEX;
    }
    x->group_id = rid & 0x8000u ? rid & 0x7fffu : 0;
    if (ne_data(x, &n, names + k * 12, &off, &bytes)) return ICONX_EFORMAT;
    return group_extract(x, off, bytes, ne_image, &n, size, out);
}

/* ---- ANI and BMP ------------------------------------------------------------- */
static int ani_frame(struct iconx *x, uint32_t *off, uint32_t *len)
{
    uint32_t riff, end, at = 12, flags = 0;
    int header = 0;
    if (get32(x, 4, &riff)) return ICONX_EFORMAT;
    end = riff > x->limit - 8 ? x->limit : riff + 8;
    for (unsigned n = 0; n < 4096 && end >= 8 && at <= end - 8; n++) {
        uint32_t id, size;
        if (get32(x, at, &id) || get32(x, at + 4, &size)) return ICONX_EFORMAT;
        uint32_t data = at + 8;
        if (size > end - data) return ICONX_EFORMAT;
        if (id == FOURCC('a', 'n', 'i', 'h')) {
            if (size < 36 || get32(x, data + 32, &flags)) return ICONX_EFORMAT;
            header = 1;
        } else if (id == FOURCC('L', 'I', 'S', 'T') && size >= 4) {
            uint32_t type;
            if (get32(x, data, &type)) return ICONX_EFORMAT;
            if (type == FOURCC('f', 'r', 'a', 'm')) {
                if (!header || !(flags & 1)) return ICONX_EFORMAT;   /* raw frames: not icon data */
                uint32_t inner = data + 4, stop = data + size;
                for (unsigned m = 0; m < 4096 && stop >= 8 && inner <= stop - 8; m++) {
                    uint32_t iid, isize;
                    if (get32(x, inner, &iid) || get32(x, inner + 4, &isize)) return ICONX_EFORMAT;
                    if (isize > stop - inner - 8) return ICONX_EFORMAT;
                    if (iid == FOURCC('i', 'c', 'o', 'n')) { *off = inner + 8; *len = isize; return ICONX_OK; }
                    inner += 8 + ((isize + 1) & ~1u);
                }
                return ICONX_ENONE;
            }
        }
        uint32_t step = (size + 1) & ~1u;
        if (step > end - data) break;
        at = data + step;
    }
    return ICONX_EFORMAT;
}
static int ani_extract(struct iconx *x, unsigned size, uint32_t *out)
{
    uint32_t off, len, base = x->base, limit = x->limit;
    int e = ani_frame(x, &off, &len);
    if (e) return e;
    x->base = base + off; x->limit = len;
    e = file_extract(x, size, out);
    x->base = base; x->limit = limit;
    return e;
}
static int bmp_extract(struct iconx *x, unsigned size, uint32_t *out)
{
    uint32_t bits;
    if (get32(x, 10, &bits)) return ICONX_EFORMAT;
    if (bits && bits >= x->limit) return ICONX_EFORMAT;
    return decode_at(x, 14, x->limit - 14, 0, bits, size, out);
}

int iconx_count(struct iconx *x)
{
    int kind = iconx_kind(x), e;
    if (kind == ICONX_ICO || kind == ICONX_CUR || kind == ICONX_ANI || kind == ICONX_BMP) return 1;
    if (kind == ICONX_PE) {
        struct pe p; uint32_t first; unsigned named, ids;
        e = pe_open(x, &p);
        if (!e) e = pe_groups(x, &p, &first, &named, &ids);
        return e == ICONX_ENONE ? 0 : e ? e : (int)(named + ids);
    }
    if (kind == ICONX_NE) {
        struct ne n; uint32_t names, count;
        e = ne_open(x, &n);
        if (!e) e = ne_type(x, &n, 0x800e, &names, &count);
        return e == ICONX_ENONE ? 0 : e ? e : (int)count;
    }
    return 0;
}

int iconx_extract(struct iconx *x, int index, unsigned size, uint32_t *argb)
{
    if (!argb || !size || size > ICONX_SIZE_MAX) return ICONX_EARG;
    x->group_id = 0;
    zero(&x->chosen, sizeof(x->chosen));
    switch (iconx_kind(x)) {
    case ICONX_ICO: case ICONX_CUR: return index ? ICONX_EINDEX : file_extract(x, size, argb);
    case ICONX_ANI: return index ? ICONX_EINDEX : ani_extract(x, size, argb);
    case ICONX_BMP: return index ? ICONX_EINDEX : bmp_extract(x, size, argb);
    case ICONX_PE: return pe_extract(x, index, size, argb);
    case ICONX_NE: return ne_extract(x, index, size, argb);
    case ICONX_DOS: return ICONX_ENONE;
    default: return ICONX_EFORMAT;
    }
}

/* ---- icon locations ---------------------------------------------------------- */
static void put(char *out, unsigned cap, unsigned *at, uint32_t c)
{
    unsigned n = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
    if (*at + n >= cap) { *at = cap; return; }   /* overflow: the caller truncates */
    if (n == 1) out[(*at)++] = (char)c;
    else {
        out[(*at)++] = (char)((n == 2 ? 0xc0 : n == 3 ? 0xe0 : 0xf0) | c >> (6 * (n - 1)));
        for (unsigned k = n - 1; k-- > 0;) out[(*at)++] = (char)(0x80 | (c >> (6 * k) & 63));
    }
}
/* Windows-1252 0x80..0x9F (the rest of the code page is Latin-1). */
static const uint16_t cp1252[32] = {
    0x20ac, 0x81, 0x201a, 0x192, 0x201e, 0x2026, 0x2020, 0x2021, 0x2c6, 0x2030, 0x160, 0x2039, 0x152, 0x8d, 0x17d, 0x8f,
    0x90, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x2dc, 0x2122, 0x161, 0x203a, 0x153, 0x9d, 0x17e, 0x178};
/* Copy a counted string (wide: UTF-16LE units; else Windows-1252 bytes) as
 * UTF-8; stops at a NUL. 0, or -1 when it does not fit. */
static int text(const uint8_t *p, uint32_t units, int wide, char *out, unsigned cap)
{
    unsigned at = 0;
    if (!cap) return -1;
    for (uint32_t k = 0; k < units && at < cap; k++) {
        uint32_t c = wide ? le16(p + 2 * k) : p[k];
        if (!c) break;
        if (wide && c >= 0xd800 && c < 0xdc00 && k + 1 < units) {
            uint32_t d = le16(p + 2 * (k + 1));
            if (d >= 0xdc00 && d < 0xe000) { c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00); k++; }
        }
        if (!wide && c >= 0x80 && c < 0xa0) c = cp1252[c - 0x80];
        if (c >= 0xd800 && c < 0xe000) c = 0xfffd;
        put(out, cap, &at, c);
    }
    if (at >= cap) { out[0] = 0; return -1; }
    out[at] = 0;
    return 0;
}
static unsigned length(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }

int iconx_link_parse(const uint8_t *p, uint32_t n, struct iconx_link *out)
{
    static const uint8_t clsid[16] = {1, 0x14, 2, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46};
    zero(out, sizeof(*out));
    if (!p) return -1;
    if (n >= 40 && p[0] == 'C' && p[1] == 'I' && p[2] == 'U' && p[3] == 'K' && p[4] == 'L' && p[5] == 'N' && p[6] == 'K' && p[7] == '3') {
        /* CiukiOS .lnk v3 (apps/desktop/shell_link.h): counted UTF-8 label,
         * target, arguments, directory, icon after a 40-byte header. */
        uint32_t at = 40;
        char *fields[5] = {0, out->target, out->arguments, 0, out->icon};
        for (unsigned k = 0; k < 5; k++) {
            uint32_t len = le32(p + 16 + k * 4);
            if (len > n - at) return -1;
            if (fields[k]) {
                if (len >= ICONX_PATH) { if (k != 2) return -1; len = ICONX_PATH - 1; }
                move(fields[k], p + at, len); fields[k][len] = 0;
                for (uint32_t j = 0; j < len; j++) if (!fields[k][j]) return -1;
            }
            at += le32(p + 16 + k * 4);
        }
        out->icon_index = (int32_t)le32(p + 12);
        out->ciuki = 1;
        return out->target[0] ? 0 : -1;
    }
    if (n < 76 || le32(p) != 76) return -1;
    for (unsigned k = 0; k < 16; k++) if (p[4 + k] != clsid[k]) return -1;
    uint32_t flags = le32(p + 20), at = 76;
    out->icon_index = (int32_t)le32(p + 56);
    if (flags & 1) {                                   /* LinkTargetIDList */
        if (n - at < 2) return -1;
        uint32_t size = le16(p + at); at += 2;
        if (size > n - at) return -1;
        at += size;
    }
    if (flags & 2) {                                   /* LinkInfo */
        if (n - at < 28) return -1;
        uint32_t size = le32(p + at), header = le32(p + at + 4), info = le32(p + at + 8);
        if (size < 28 || size > n - at || header < 28 || header > size) return -1;
        if (info & 1) {
            const uint8_t *li = p + at;
            uint32_t base = le32(li + 16), suffix = le32(li + 24);
            int wide = 0, wide_suffix = 0;
            if (header >= 36 && le32(li + 28)) { base = le32(li + 28); wide = 1; }
            if (header >= 36 && le32(li + 32)) { suffix = le32(li + 32); wide_suffix = 1; }
            if (base < header || base >= size || (suffix && (suffix < header || suffix >= size))) return -1;
            if (text(li + base, (size - base) / (wide ? 2 : 1), wide, out->target, ICONX_PATH)) return -1;
            char tail[ICONX_PATH];
            tail[0] = 0;
            if (suffix && text(li + suffix, (size - suffix) / (wide_suffix ? 2 : 1), wide_suffix, tail, ICONX_PATH)) return -1;
            unsigned a = length(out->target), b = length(tail);
            int present = b <= a;
            for (unsigned k = 0; present && k < b; k++) if (lower(out->target[a - b + k]) != lower(tail[k])) present = 0;
            if (b && !present) {   /* LocalBasePath may already end with the suffix */
                if (a + b + 2 > ICONX_PATH) return -1;
                if (a && out->target[a - 1] != '\\' && out->target[a - 1] != '/') out->target[a++] = '\\';
                move(out->target + a, tail, b + 1);
            }
        }
        at += size;
    }
    char relative[ICONX_PATH], scratch[ICONX_PATH];
    relative[0] = 0;
    char *fields[5] = {scratch, relative, scratch, out->arguments, out->icon};
    int wide = (flags & 0x80) != 0;
    for (unsigned k = 0; k < 5; k++) if (flags & (4u << k)) {
        if (n - at < 2) return -1;
        uint32_t units = le16(p + at), bytes = units * (wide ? 2 : 1);
        at += 2;
        if (bytes > n - at) return -1;
        if (text(p + at, units, wide, fields[k], ICONX_PATH) && (k == 1 || k == 4)) return -1;
        at += bytes;
    }
    if (!out->target[0] && relative[0]) move(out->target, relative, length(relative) + 1);
    if (flags & 0x4000)                                /* HasExpIcon: IconEnvironmentDataBlock */
        for (unsigned blocks = 0; blocks < 64 && n - at >= 8; blocks++) {
            uint32_t size = le32(p + at), signature = le32(p + at + 4);
            if (size < 8 || size > n - at) break;
            if (signature == 0xa0000007u && size >= 788) {
                char expanded[ICONX_PATH];
                if (!text(p + at + 268, 260, 1, expanded, ICONX_PATH) && expanded[0])
                    move(out->icon, expanded, length(expanded) + 1);
                else if (!text(p + at + 8, 260, 0, expanded, ICONX_PATH) && expanded[0])
                    move(out->icon, expanded, length(expanded) + 1);
                break;
            }
            at += size;
        }
    return out->target[0] || out->icon[0] ? 0 : -1;
}

static int same_name(const char *a, const char *b)
{
    while (*a && *b) if (lower((unsigned char)*a++) != lower((unsigned char)*b++)) return 0;
    return !*a && !*b;
}
int iconx_pif_parse(const uint8_t *p, uint32_t n, char *icon, unsigned cap, int32_t *index, char *program, unsigned program_cap)
{
    if (icon && cap) icon[0] = 0;
    if (program && program_cap) program[0] = 0;
    if (index) *index = 0;
    if (!p || !icon || !cap || !index || n < 0x171) return -1;
    if (program && program_cap && text(p + 0x24, 63, 0, program, program_cap)) program[0] = 0;
    if (program) for (unsigned k = length(program); k && program[k - 1] == ' '; k--) program[k - 1] = 0;
    if (n < 0x171 + 22) return 0;
    static const char pifex[] = "MICROSOFT PIFEX";
    for (unsigned k = 0; k < 15; k++) if (p[0x171 + k] != (uint8_t)pifex[k]) return -1;
    uint32_t at = le16(p + 0x171 + 16);
    for (unsigned sections = 0; sections < 64 && at != 0xffff; sections++) {
        if (at < 0x171 || at > n - 22) return -1;
        uint32_t data = le16(p + at + 18), size = le16(p + at + 20);
        static const char vmm[] = "WINDOWS VMM 4.0";
        int match = 1;
        for (unsigned k = 0; k < 15; k++) if (p[at + k] != (uint8_t)vmm[k]) match = 0;
        if (match) {
            if (size < 0xaa || data > n || n - data < 0xaa) return -1;
            if (text(p + data + 0x58, 80, 0, icon, cap)) { icon[0] = 0; return -1; }
            *index = (int32_t)le16(p + data + 0xa8);
            const char *base = icon;
            for (const char *s = icon; *s; s++) if (*s == '\\' || *s == '/' || *s == ':') base = s + 1;
            if (!icon[0] || same_name(base, "PIFMGR.DLL")) { icon[0] = 0; *index = 0; return 0; }
            return 1;
        }
        at = le16(p + at + 16);
    }
    return 0;
}

/* One code point of an INI file; wide: UTF-16LE after its BOM. */
static uint32_t ini_char(const uint8_t *p, uint32_t n, uint32_t *at, int wide)
{
    if (wide) {
        if (n - *at < 2) { *at = n; return 0; }
        uint32_t c = le16(p + *at); *at += 2;
        if (c >= 0xd800 && c < 0xdc00 && n - *at >= 2) {
            uint32_t d = le16(p + *at);
            if (d >= 0xdc00 && d < 0xe000) { *at += 2; return 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00); }
        }
        return c >= 0xd800 && c < 0xe000 ? 0xfffd : c;
    }
    return p[(*at)++];   /* bytes are copied through: ANSI and UTF-8 stay as they are */
}
int iconx_ini_get(const uint8_t *p, uint32_t n, const char *section, const char *key, char *out, unsigned cap)
{
    if (!p || !section || !key || !out || !cap) return 0;
    out[0] = 0;
    uint32_t at = 0;
    int wide = n >= 2 && p[0] == 0xff && p[1] == 0xfe;
    if (wide) at = 2;
    else if (n >= 3 && p[0] == 0xef && p[1] == 0xbb && p[2] == 0xbf) at = 3;
    int inside = 0;
    for (unsigned lines = 0; at < n && lines < 4096; lines++) {
        char line[512];
        unsigned len = 0;
        int too_long = 0;
        while (at < n) {
            uint32_t c = ini_char(p, n, &at, wide);
            if (c == '\n' || !c) break;
            if (c == '\r') continue;
            if (wide) { unsigned before = len; put(line, sizeof(line), &len, c); if (len >= sizeof(line)) { len = before; too_long = 1; } }
            else if (len + 1 < sizeof(line)) line[len++] = (char)c;
            else too_long = 1;
        }
        line[len] = 0;
        unsigned b = 0;
        while (line[b] == ' ' || line[b] == '\t') b++;
        while (len > b && (line[len - 1] == ' ' || line[len - 1] == '\t')) line[--len] = 0;
        if (line[b] == '[') {
            char *close = line + b + 1;
            while (*close && *close != ']') close++;
            *close = 0;
            inside = same_name(line + b + 1, section);
            continue;
        }
        if (!inside || too_long || line[b] == ';') continue;
        char *eq = line + b;
        while (*eq && *eq != '=') eq++;
        if (!*eq) continue;
        char *k_end = eq;
        while (k_end > line + b && (k_end[-1] == ' ' || k_end[-1] == '\t')) k_end--;
        char saved = *k_end; *k_end = 0;
        int hit = same_name(line + b, key);
        *k_end = saved;
        if (!hit) continue;
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        unsigned vl = length(v);
        if (vl >= 2 && v[0] == '"' && v[vl - 1] == '"') { v++; vl -= 2; }
        if (vl >= cap) return 0;
        move(out, v, vl); out[vl] = 0;
        return 1;
    }
    return 0;
}

void iconx_location_split(char *s, int32_t *index)
{
    if (index) *index = 0;
    if (!s) return;
    unsigned n = length(s), comma = n;
    while (comma && s[comma - 1] != ',') comma--;
    if (comma) {
        const char *q = s + comma;
        while (*q == ' ' || *q == '\t') q++;
        int negative = *q == '-';
        if (negative || *q == '+') q++;
        uint32_t value = 0; int digits = 0, overflow = 0;
        while (*q >= '0' && *q <= '9') {
            if (value > 214748364u || (value == 214748364u && *q > '7')) overflow = 1;
            else value = value * 10 + (uint32_t)(*q - '0');
            q++; digits++;
        }
        while (*q == ' ' || *q == '\t') q++;
        if (digits && !*q && !overflow) {
            if (index) *index = negative ? -(int32_t)value : (int32_t)value;
            s[comma - 1] = 0; n = comma - 1;
        }
    }
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    unsigned b = 0;
    while (s[b] == ' ' || s[b] == '\t') b++;
    if (s[b] == '"' && n > b + 1 && s[n - 1] == '"') { s[--n] = 0; b++; }
    if (b) move(s, s + b, n - b + 1);
}

int iconx_desktop_ini(const uint8_t *p, uint32_t n, char *icon, unsigned cap, int32_t *index)
{
    char value[32];
    if (index) *index = 0;
    if (iconx_ini_get(p, n, ".ShellClassInfo", "IconFile", icon, cap) && icon[0]) {
        if (iconx_ini_get(p, n, ".ShellClassInfo", "IconIndex", value, sizeof(value))) {
            char probe[48] = "x,";
            move(probe + 2, value, length(value) + 1);
            iconx_location_split(probe, index);
        }
        return 1;
    }
    if (iconx_ini_get(p, n, ".ShellClassInfo", "IconResource", icon, cap) && icon[0]) {
        iconx_location_split(icon, index);
        return icon[0] != 0;
    }
    return 0;
}

int iconx_autorun_inf(const uint8_t *p, uint32_t n, char *icon, unsigned cap, int32_t *index)
{
    if (index) *index = 0;
    if (!iconx_ini_get(p, n, "autorun", "icon", icon, cap) || !icon[0]) return 0;
    iconx_location_split(icon, index);
    return icon[0] != 0;
}
