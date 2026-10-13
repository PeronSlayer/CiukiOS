/* SPDX-License-Identifier: MIT */
#include <ciuki/font.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct raster_alloc { int failed; };
static void *raster_malloc(size_t n, void *ctx)
{
    void *p = malloc(n);
    if (!p && ctx) ((struct raster_alloc *)ctx)->failed = 1;
    return p;
}
#define STBTT_malloc(n, ctx) raster_malloc(n, ctx)
#define STBTT_free(p, ctx) ((void)(ctx), free(p))
#define STBTT_RASTERIZER_VERSION 1
#define STB_TRUETYPE_IMPLEMENTATION
#include "../../third_party/stb/stb_truetype.h"

#define FONT_FILE_MAX (8u * 1024u * 1024u)
#define GLYPH_BYTES_MAX (64u * 1024u)
struct cache_entry {
    unsigned glyph, size;
    uint64_t used;
    int valid;
    struct ciuki_font_bitmap bitmap;
};
struct ciuki_font_face {
    pthread_mutex_t mutex;
    uint8_t *data;
    size_t length;
    stbtt_fontinfo stb;
    struct raster_alloc alloc;
    struct ciuki_font_metrics metrics;
    struct ciuki_font_cache_stats stats;
    uint64_t clock;
    struct cache_entry cache[CIUKI_FONT_CACHE_GLYPHS];
};
static int fail(int e) { errno = e; return -1; }
static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)be16(p) << 16 | be16(p + 2); }
static int signed16(const uint8_t *p) { unsigned v = be16(p); return v < 32768 ? (int)v : (int)v - 65536; }
static int range(size_t total, size_t off, size_t n) { return off <= total && n <= total - off; }
static const uint8_t *table(struct ciuki_font_face *f, const char *tag, size_t minimum, size_t *length)
{
    unsigned count = be16(f->data + 4);
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *r = f->data + 12 + i * 16;
        if (!memcmp(r, tag, 4)) {
            size_t off = be32(r + 8), n = be32(r + 12);
            if (n < minimum || !range(f->length, off, n)) return NULL;
            if (length) *length = n;
            return f->data + off;
        }
    }
    return NULL;
}
static unsigned utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xc0 | cp >> 6); out[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | cp >> 12); out[1] = (char)(0x80 | (cp >> 6 & 63)); out[2] = (char)(0x80 | (cp & 63)); return 3;
    }
    out[0] = (char)(0xf0 | cp >> 18); out[1] = (char)(0x80 | (cp >> 12 & 63));
    out[2] = (char)(0x80 | (cp >> 6 & 63)); out[3] = (char)(0x80 | (cp & 63)); return 4;
}
static void font_name(struct ciuki_font_face *f, unsigned id, char *out)
{
    size_t length;
    const uint8_t *t = table(f, "name", 6, &length);
    int best = -1;
    if (!t || !range(length, 6, (size_t)be16(t + 2) * 12)) return;
    size_t strings = be16(t + 4);
    for (unsigned i = 0; i < be16(t + 2); i++) {
        const uint8_t *r = t + 6 + i * 12;
        unsigned platform = be16(r), encoding = be16(r + 2), language = be16(r + 4);
        if (be16(r + 6) != id || !(platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10)))) continue;
        int score = platform == 3 ? (language == 0x409 ? 3 : 2) : 1;
        size_t off = strings + be16(r + 10), n = be16(r + 8), used = 0;
        if (score <= best || (n & 1) || !range(length, off, n)) continue;
        for (size_t j = 0; j < n; j += 2) {
            uint32_t cp = be16(t + off + j);
            if (cp >= 0xd800 && cp <= 0xdbff && j + 3 < n) {
                unsigned lo = be16(t + off + j + 2);
                if (lo >= 0xdc00 && lo <= 0xdfff) { cp = 0x10000 + ((cp - 0xd800) << 10) + lo - 0xdc00; j += 2; }
            }
            if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd;
            if (!cp) cp = 0xfffd;
            char encoded[4]; unsigned bytes = utf8_encode(cp, encoded);
            if (used + bytes >= CIUKI_FONT_NAME_MAX) break;
            memcpy(out + used, encoded, bytes); used += bytes;
        }
        out[used] = 0; best = score;
    }
}
/* This bounded sfnt envelope is corruption detection, not a hostile-font
 * validator. stb's outline parser must only see trusted fonts. */
static int initialise(struct ciuki_font_face *f)
{
    if (f->length < 12 || be32(f->data) != 0x10000 || !be16(f->data + 4) ||
        !range(f->length, 12, (size_t)be16(f->data + 4) * 16)) return fail(EINVAL);
    for (unsigned i = 0; i < be16(f->data + 4); i++) {
        const uint8_t *r = f->data + 12 + i * 16;
        if (!range(f->length, be32(r + 8), be32(r + 12))) return fail(EINVAL);
    }
    const uint8_t *head = table(f, "head", 54, NULL), *hhea = table(f, "hhea", 36, NULL), *maxp = table(f, "maxp", 6, NULL);
    if (!head || !hhea || !maxp || !table(f, "cmap", 4, NULL) || !table(f, "glyf", 0, NULL)) return fail(EINVAL);
    unsigned upem = be16(head + 18), glyphs = be16(maxp + 4), hm = be16(hhea + 34), fmt = be16(head + 50);
    if (upem < 16 || upem > 16384 || !glyphs || !hm || hm > glyphs || fmt > 1 ||
        !table(f, "hmtx", hm * 4u + (glyphs - hm) * 2u, NULL) ||
        !table(f, "loca", (glyphs + 1u) * (fmt ? 4u : 2u), NULL)) return fail(EINVAL);
    f->stb.userdata = &f->alloc;
    if (!stbtt_InitFont(&f->stb, f->data, 0)) return fail(EINVAL);
    /* This contract uses kern only. Do not let GPOS override the legacy table. */
    f->stb.gpos = 0;
    f->metrics.units_per_em = upem; f->metrics.glyph_count = glyphs;
    f->metrics.ascent = signed16(hhea + 4); f->metrics.descent = signed16(hhea + 6); f->metrics.line_gap = signed16(hhea + 8);
    const uint8_t *os2 = table(f, "OS/2", 78, NULL);
    if (os2) {
        f->metrics.has_os2 = 1;
        f->metrics.weight = be16(os2 + 4); f->metrics.selection = be16(os2 + 62);
        f->metrics.typo_ascent = signed16(os2 + 68); f->metrics.typo_descent = signed16(os2 + 70);
        f->metrics.typo_line_gap = signed16(os2 + 72); f->metrics.win_ascent = be16(os2 + 74); f->metrics.win_descent = be16(os2 + 76);
    }
    font_name(f, 1, f->metrics.family); font_name(f, 2, f->metrics.style);
    return 0;
}
int ciuki_font_open(const char *path, struct ciuki_font_face **out)
{
    if (!out) return fail(EINVAL);
    *out = NULL;
    if (!path) return fail(EINVAL);
    FILE *stream = fopen(path, "rb");
    if (!stream) return -1;
    if (fseek(stream, 0, SEEK_END)) { int e = errno; fclose(stream); return fail(e); }
    long n = ftell(stream);
    if (n < 12 || (unsigned long)n > FONT_FILE_MAX) { fclose(stream); return fail(EINVAL); }
    rewind(stream);
    struct ciuki_font_face *f = calloc(1, sizeof(*f));
    if (!f) { fclose(stream); return fail(ENOMEM); }
    f->data = malloc((size_t)n); f->length = (size_t)n;
    if (!f->data) { fclose(stream); free(f); return fail(ENOMEM); }
    if (fread(f->data, 1, f->length, stream) != f->length) { int e = ferror(stream) ? EIO : EINVAL; fclose(stream); free(f->data); free(f); return fail(e); }
    fclose(stream);
    if (initialise(f)) { int e = errno; free(f->data); free(f); return fail(e); }
    int e = pthread_mutex_init(&f->mutex, NULL);
    if (e) { free(f->data); free(f); return fail(e); }
    *out = f; return 0;
}
static void clear_locked(struct ciuki_font_face *f)
{
    for (unsigned i = 0; i < CIUKI_FONT_CACHE_GLYPHS; i++) { free(f->cache[i].bitmap.coverage); memset(&f->cache[i], 0, sizeof(f->cache[i])); }
    f->stats.glyphs = 0; f->stats.bytes = 0;
}
void ciuki_font_close(struct ciuki_font_face *f)
{
    if (!f) return;
    clear_locked(f); pthread_mutex_destroy(&f->mutex); free(f->data); free(f);
}
int ciuki_font_get_metrics(struct ciuki_font_face *f, struct ciuki_font_metrics *out)
{
    if (!f || !out) return fail(EINVAL);
    pthread_mutex_lock(&f->mutex); *out = f->metrics; pthread_mutex_unlock(&f->mutex); return 0;
}
static int scalar(uint32_t cp) { return cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff); }
int ciuki_font_lookup(struct ciuki_font_face *f, uint32_t cp, unsigned *out)
{
    if (!f || !out || !scalar(cp)) return fail(EINVAL);
    pthread_mutex_lock(&f->mutex); *out = (unsigned)stbtt_FindGlyphIndex(&f->stb, (int)cp); pthread_mutex_unlock(&f->mutex); return 0;
}
int ciuki_font_hmetrics(struct ciuki_font_face *f, unsigned glyph, int *advance, int *bearing)
{
    if (!f || !advance || !bearing || glyph >= f->metrics.glyph_count) return fail(EINVAL);
    pthread_mutex_lock(&f->mutex); stbtt_GetGlyphHMetrics(&f->stb, (int)glyph, advance, bearing); pthread_mutex_unlock(&f->mutex); return 0;
}
static int size_valid(unsigned size) { return size >= 64 && size <= 128 * 64; }
static int32_t scaled(struct ciuki_font_face *f, int units, unsigned size)
{
    int64_t v = (int64_t)units * size, half = f->metrics.units_per_em / 2;
    return (int32_t)((v < 0 ? v - half : v + half) / f->metrics.units_per_em);
}
static struct cache_entry *oldest(struct ciuki_font_face *f)
{
    struct cache_entry *result = NULL;
    for (unsigned i = 0; i < CIUKI_FONT_CACHE_GLYPHS; i++) if (f->cache[i].valid && (!result || f->cache[i].used < result->used)) result = &f->cache[i];
    return result;
}
static void evict(struct ciuki_font_face *f, struct cache_entry *e)
{
    f->stats.bytes -= (size_t)e->bitmap.width * e->bitmap.height; f->stats.glyphs--; f->stats.evictions++;
    free(e->bitmap.coverage); memset(e, 0, sizeof(*e));
}
static struct ciuki_font_bitmap *cached(struct ciuki_font_face *f, unsigned glyph, unsigned size)
{
    for (unsigned i = 0; i < CIUKI_FONT_CACHE_GLYPHS; i++) {
        struct cache_entry *e = &f->cache[i];
        if (e->valid && e->glyph == glyph && e->size == size) { e->used = ++f->clock; f->stats.hits++; return &e->bitmap; }
    }
    f->stats.misses++;
    float scale = (float)size / (64.0f * f->metrics.units_per_em);
    int x0, y0, x1, y1, advance, bearing;
    stbtt_GetGlyphBitmapBox(&f->stb, (int)glyph, scale, scale, &x0, &y0, &x1, &y1);
    if (x1 < x0 || y1 < y0 || x1 - x0 > 512 || y1 - y0 > 512 || (size_t)(x1 - x0) * (y1 - y0) > GLYPH_BYTES_MAX) { fail(EOVERFLOW); return NULL; }
    size_t bytes = (size_t)(x1 - x0) * (y1 - y0);
    uint8_t *pixels = bytes ? calloc(bytes, 1) : NULL;
    if (bytes && !pixels) { fail(ENOMEM); return NULL; }
    f->alloc.failed = 0;
    if (bytes) stbtt_MakeGlyphBitmap(&f->stb, pixels, x1 - x0, y1 - y0, x1 - x0, scale, scale, (int)glyph);
    if (f->alloc.failed) { free(pixels); fail(ENOMEM); return NULL; }
    while (f->stats.glyphs == CIUKI_FONT_CACHE_GLYPHS || bytes > CIUKI_FONT_CACHE_BYTES - f->stats.bytes) evict(f, oldest(f));
    struct cache_entry *entry = NULL;
    for (unsigned i = 0; i < CIUKI_FONT_CACHE_GLYPHS; i++) if (!f->cache[i].valid) { entry = &f->cache[i]; break; }
    stbtt_GetGlyphHMetrics(&f->stb, (int)glyph, &advance, &bearing);
    entry->glyph = glyph; entry->size = size; entry->valid = 1; entry->used = ++f->clock;
    entry->bitmap = (struct ciuki_font_bitmap){(unsigned)(x1 - x0), (unsigned)(y1 - y0), (unsigned)(x1 - x0), x0, y0, scaled(f, advance, size), pixels};
    f->stats.bytes += bytes; f->stats.glyphs++;
    return &entry->bitmap;
}
int ciuki_font_raster(struct ciuki_font_face *f, unsigned glyph, unsigned size, struct ciuki_font_bitmap *out)
{
    if (!f || !out || glyph >= f->metrics.glyph_count || !size_valid(size)) return fail(EINVAL);
    memset(out, 0, sizeof(*out)); pthread_mutex_lock(&f->mutex);
    struct ciuki_font_bitmap *b = cached(f, glyph, size);
    int result = -1;
    if (b) {
        size_t n = (size_t)b->width * b->height;
        uint8_t *copy = n ? malloc(n) : NULL;
        if (!n || copy) { *out = *b; out->coverage = copy; if (n) memcpy(copy, b->coverage, n); result = 0; }
        else fail(ENOMEM);
    }
    pthread_mutex_unlock(&f->mutex); return result;
}
void ciuki_font_bitmap_free(struct ciuki_font_bitmap *b) { if (b) { free(b->coverage); memset(b, 0, sizeof(*b)); } }
static uint32_t decode(const unsigned char **p, size_t *left)
{
    const unsigned char *s = *p;
    unsigned c = s[0], n = c < 0x80 ? 1 : c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
    uint32_t cp = n == 1 ? c : n == 2 ? c & 31 : n == 3 ? c & 15 : c & 7;
    int valid = n && n <= *left;
    for (unsigned i = 1; valid && i < n; i++) {
        if ((s[i] & 0xc0) != 0x80) valid = 0;
        else cp = cp << 6 | (s[i] & 63);
    }
    if (!valid || !scalar(cp) || (n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000)) { n = 1; cp = 0xfffd; }
    *p += n; *left -= n; return cp;
}
static int run(struct ciuki_font_face *f, unsigned size, const char *text, size_t length,
               int x, int baseline, ciuki_font_painter painter, void *ctx, int32_t *out)
{
    if (!f || (!text && length) || !size_valid(size) || !out) return fail(EINVAL);
    pthread_mutex_lock(&f->mutex);
    const unsigned char *s = (const unsigned char *)text;
    int64_t pen = 0; int previous = -1, result = 0;
    while (length) {
        unsigned glyph = (unsigned)stbtt_FindGlyphIndex(&f->stb, (int)decode(&s, &length));
        if (previous >= 0) pen += scaled(f, stbtt_GetGlyphKernAdvance(&f->stb, previous, (int)glyph), size);
        int advance, bearing;
        stbtt_GetGlyphHMetrics(&f->stb, (int)glyph, &advance, &bearing);
        if (pen < INT32_MIN || pen > INT32_MAX) { result = fail(EOVERFLOW); break; }
        if (painter) {
            struct ciuki_font_bitmap *b = cached(f, glyph, size);
            if (!b) { result = -1; break; }
            int64_t gx = x + (pen < 0 ? pen - 32 : pen + 32) / 64 + b->left, gy = (int64_t)baseline + b->top;
            if (gx < INT_MIN || gx > INT_MAX || gy < INT_MIN || gy > INT_MAX) { result = fail(EOVERFLOW); break; }
            if (b->width && b->height) { result = painter(ctx, (int)gx, (int)gy, b); if (result) break; }
        }
        pen += scaled(f, advance, size); previous = (int)glyph;
    }
    if (!result && (pen < INT32_MIN || pen > INT32_MAX)) result = fail(EOVERFLOW);
    if (!result) *out = (int32_t)pen;
    pthread_mutex_unlock(&f->mutex); return result;
}
int ciuki_font_measure(struct ciuki_font_face *f, unsigned size, const char *s, size_t n, int32_t *out)
{ return run(f, size, s, n, 0, 0, NULL, NULL, out); }
int ciuki_font_paint(struct ciuki_font_face *f, unsigned size, const char *s, size_t n, int x, int baseline,
                     ciuki_font_painter painter, void *ctx, int32_t *out)
{ if (!painter) return fail(EINVAL); return run(f, size, s, n, x, baseline, painter, ctx, out); }
int ciuki_font_cache_info(struct ciuki_font_face *f, struct ciuki_font_cache_stats *out)
{
    if (!f || !out) return fail(EINVAL);
    pthread_mutex_lock(&f->mutex); *out = f->stats; pthread_mutex_unlock(&f->mutex); return 0;
}
void ciuki_font_cache_clear(struct ciuki_font_face *f)
{ if (f) { pthread_mutex_lock(&f->mutex); clear_locked(f); pthread_mutex_unlock(&f->mutex); } }
