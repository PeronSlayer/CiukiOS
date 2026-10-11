/* SPDX-License-Identifier: MIT
 * Tango 0.8.90 system icon store reader (CIUKICS1).
 *
 * The store is built by apps/desktop/convert_icons.py from the pinned Tango
 * archive (Public Domain) and installed as /system/icons/tango.cis; its
 * format is documented in assets/icons/README.md. Only the index is kept in
 * memory: each icon is read and decoded on first use (QOI 1.0 chunk stream,
 * https://qoiformat.org/qoi-specification.pdf, or the integer area filter of
 * another stored image) and stays cached for the life of the process.
 * "ciuki" is the identity icon: the approved portrait conversion
 * (/system/assets/ciuki-portrait.xrgb, pinned by convert_portrait.py)
 * reduced with the same area filter. Integer arithmetic only, 32-bit
 * intermediates; not thread-safe (call from the UI thread). */
#include "deskui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DU_ICON_STORE_PATH
#define DU_ICON_STORE_PATH "/system/icons/tango.cis"
#endif
#ifndef DU_ICON_PORTRAIT_PATH
#define DU_ICON_PORTRAIT_PATH "/system/assets/ciuki-portrait.xrgb"
#endif

#define HEADER 48u
#define ENTRY 16u
#define NONE 0xffffu
#define INDEX_MAX (1u << 20)        /* names, images and strings */
#define DATA_MAX (64u << 20)
#define PORTRAIT 256u

struct cached { struct du_icon icon; uint32_t pixels[]; };

static const unsigned sizes[4] = {16, 22, 32, 48};
static struct du_icon unavailable;   /* negative cache marker */
static struct {
    int state;                      /* 0 not opened, 1 open, -1 unavailable */
    FILE *file;
    uint8_t *index;                 /* store bytes [0, data offset) */
    uint32_t images_off, strings_off, strings_len, data_off, data_len, notice;
    unsigned names, images;
    const struct du_icon **cache;   /* names * 4 */
    const struct du_icon *ciuki[4];
    char store[256], portrait[256];
} S;

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int slot_of(unsigned size)
{
    for (int i = 0; i < 4; i++) if (sizes[i] == size) return i;
    return -1;
}

int du_icon_paths(const char *store, const char *portrait)
{
    if (S.state || S.ciuki[0] || S.ciuki[1] || S.ciuki[2] || S.ciuki[3]) return -1;
    if ((store && strlen(store) >= sizeof(S.store)) || (portrait && strlen(portrait) >= sizeof(S.portrait))) return -1;
    if (store) strcpy(S.store, store);
    if (portrait) strcpy(S.portrait, portrait);
    return 0;
}

static int open_store(void)
{
    if (S.state) return S.state > 0 ? 0 : -1;
    S.state = -1;
    FILE *f = fopen(S.store[0] ? S.store : DU_ICON_STORE_PATH, "rb");
    uint8_t h[HEADER];
    if (!f) return -1;
    if (fread(h, 1, HEADER, f) != HEADER || memcmp(h, "CIUKICS1", 8) || le16(h + 8) != 1 || le16(h + 10) != HEADER ||
        h[16] != 16 || h[17] != 22 || h[18] != 32 || h[19] != 48) goto fail;
    unsigned names = le16(h + 12), images = le16(h + 14);
    uint32_t names_off = le32(h + 20), images_off = le32(h + 24), strings_off = le32(h + 28);
    uint32_t strings_len = le32(h + 32), data_off = le32(h + 36), data_len = le32(h + 40), notice = le32(h + 44);
    if (!names || !images || names >= NONE || images >= NONE || names_off != HEADER ||
        images_off != names_off + ENTRY * names || strings_off != images_off + ENTRY * images ||
        !strings_len || strings_len > INDEX_MAX || data_off > INDEX_MAX || strings_off + strings_len > data_off ||
        data_len > DATA_MAX || notice >= strings_len) goto fail;
    long end;
    if (fseek(f, 0, SEEK_END) || (end = ftell(f)) < 0 || (uint32_t)end != data_off + data_len || fseek(f, 0, SEEK_SET)) goto fail;
    uint8_t *index = malloc(data_off);
    const struct du_icon **cache = calloc((size_t)names * 4, sizeof(*cache));
    if (!index || !cache || fread(index, 1, data_off, f) != data_off || index[strings_off + strings_len - 1]) {
        free(index); free(cache); goto fail;
    }
    /* Every name and context offset must point inside the NUL-terminated pool. */
    for (unsigned i = 0; i < names; i++) {
        const uint8_t *e = index + names_off + ENTRY * i;
        if (le32(e) >= strings_len || le32(e + 4) >= strings_len) { free(index); free(cache); goto fail; }
    }
    S.file = f; S.index = index; S.cache = cache; S.names = names; S.images = images;
    S.images_off = images_off; S.strings_off = strings_off; S.strings_len = strings_len;
    S.data_off = data_off; S.data_len = data_len; S.notice = notice;
    S.state = 1;
    return 0;
fail:
    fclose(f);
    return -1;
}

static const char *string_at(uint32_t off) { return (const char *)S.index + S.strings_off + off; }
static const uint8_t *name_entry(unsigned i) { return S.index + HEADER + ENTRY * i; }
static const uint8_t *image_entry(unsigned i) { return S.index + S.images_off + ENTRY * i; }

/* Binary search of the sorted name table; "context/name" also matches. */
static int find(const char *name)
{
    char bare[96];
    const char *slash = strchr(name, '/'), *key = name;
    size_t context = 0;
    if (slash) {
        context = (size_t)(slash - name);
        if (strlen(slash + 1) >= sizeof(bare)) return -1;
        strcpy(bare, slash + 1); key = bare;
    }
    unsigned lo = 0, hi = S.names;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        int c = strcmp(key, string_at(le32(name_entry(mid))));
        if (!c) {
            const char *ctx = string_at(le32(name_entry(mid) + 4));
            if (slash && (strlen(ctx) != context || memcmp(ctx, name, context))) return -1;
            return (int)mid;
        }
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return -1;
}

static int qoi(const uint8_t *in, uint32_t len, uint32_t *out, uint32_t count)
{
    uint32_t index[64] = {0}, px = 0xff000000u, p = 0, run = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (run) run--;
        else {
            if (p >= len) return -1;
            unsigned b = in[p++];
            int r = (int)(px >> 16 & 255), g = (int)(px >> 8 & 255), bl = (int)(px & 255), a = (int)(px >> 24);
            if (b == 0xfe) {
                if (len - p < 3) return -1;
                r = in[p]; g = in[p + 1]; bl = in[p + 2]; p += 3;
            } else if (b == 0xff) {
                if (len - p < 4) return -1;
                r = in[p]; g = in[p + 1]; bl = in[p + 2]; a = in[p + 3]; p += 4;
            } else if (b >> 6 == 0) {
                uint32_t q = index[b];
                r = (int)(q >> 16 & 255); g = (int)(q >> 8 & 255); bl = (int)(q & 255); a = (int)(q >> 24);
            } else if (b >> 6 == 1) {
                r = (r + (int)(b >> 4 & 3) - 2) & 255; g = (g + (int)(b >> 2 & 3) - 2) & 255; bl = (bl + (int)(b & 3) - 2) & 255;
            } else if (b >> 6 == 2) {
                if (p >= len) return -1;
                unsigned b2 = in[p++];
                int dg = (int)(b & 63) - 32;
                r = (r + dg + (int)(b2 >> 4) - 8) & 255; g = (g + dg) & 255; bl = (bl + dg + (int)(b2 & 15) - 8) & 255;
            } else run = b & 63;
            px = (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)bl;
            index[((unsigned)r * 3 + (unsigned)g * 5 + (unsigned)bl * 7 + (unsigned)a * 11) & 63] = px;
        }
        out[i] = px;
    }
    return p == len && !run ? 0 : -1;
}

/* Integer area filter, src x src -> dst x dst, both at most 256: the overlap-
 * and alpha-weighted average (premultiplied), rounded half up. With 8-bit
 * channels every sum stays below 2^32 (255 * 255 * 256 * 256 + rounding). */
static void area(const uint32_t *src, unsigned s, uint32_t *dst, unsigned d)
{
    uint32_t total = (uint32_t)s * s;
    for (unsigned dy = 0; dy < d; dy++) for (unsigned dx = 0; dx < d; dx++) {
        uint32_t sa = 0, sr = 0, sg = 0, sb = 0;
        unsigned ylo = dy * s, yhi = ylo + s, xlo = dx * s, xhi = xlo + s;
        for (unsigned sy = ylo / d; sy <= (yhi - 1) / d; sy++) {
            unsigned top = sy * d > ylo ? sy * d : ylo, bottom = (sy + 1) * d < yhi ? (sy + 1) * d : yhi;
            for (unsigned sx = xlo / d; sx <= (xhi - 1) / d; sx++) {
                unsigned left = sx * d > xlo ? sx * d : xlo, right = (sx + 1) * d < xhi ? (sx + 1) * d : xhi;
                uint32_t p = src[sy * s + sx], wa = (uint32_t)(right - left) * (bottom - top) * (p >> 24);
                sa += wa; sr += wa * (p >> 16 & 255); sg += wa * (p >> 8 & 255); sb += wa * (p & 255);
            }
        }
        uint32_t a = (sa + total / 2) / total;
        dst[dy * d + dx] = a ? a << 24 | ((sr + sa / 2) / sa) << 16 | ((sg + sa / 2) / sa) << 8 | (sb + sa / 2) / sa : 0;
    }
}

static uint32_t fnv(const uint32_t *px, uint32_t count)
{
    uint32_t h = 0x811c9dc5u;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t bytes[4] = {(uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i], (uint8_t)(px[i] >> 24)};
        for (int k = 0; k < 4; k++) h = (h ^ bytes[k]) * 0x01000193u;
    }
    return h;
}

/* Decode stored image k (size x size) into out; scaled images recurse once. */
static int decode(unsigned k, uint32_t *out, unsigned size, int depth)
{
    const uint8_t *e = image_entry(k);
    unsigned encoding = e[1], source = le16(e + 2);
    uint32_t off = le32(e + 4), len = le32(e + 8), count = (uint32_t)size * size;
    if (e[0] != size) return -1;
    if (encoding == 0) {
        if (off > S.data_len || len > S.data_len - off || !len || len > count * 5) return -1;
        uint8_t *buffer = malloc(len);
        int ok = buffer && !fseek(S.file, (long)(S.data_off + off), SEEK_SET) && fread(buffer, 1, len, S.file) == len &&
                 !qoi(buffer, len, out, count);
        free(buffer);
        if (!ok) return -1;
    } else if (encoding == 1 && !depth && source < S.images) {
        unsigned from = image_entry(source)[0];
        uint32_t *temp = from ? malloc((size_t)from * from * 4) : NULL;
        int ok = temp && image_entry(source)[1] == 0 && !decode(source, temp, from, 1);
        if (ok) area(temp, from, out, size);
        free(temp);
        if (!ok) return -1;
    } else return -1;
    return fnv(out, count) == le32(e + 12) ? 0 : -1;
}

static const struct du_icon *ciuki(int slot)
{
    if (S.ciuki[slot]) return S.ciuki[slot] == &unavailable ? NULL : S.ciuki[slot];
    unsigned size = sizes[slot];
    uint32_t *portrait = malloc(PORTRAIT * PORTRAIT * 4);
    struct cached *icon = malloc(sizeof(*icon) + (size_t)size * size * 4);
    FILE *f = fopen(S.portrait[0] ? S.portrait : DU_ICON_PORTRAIT_PATH, "rb");
    int ok = portrait && icon && f && fread(portrait, 4, PORTRAIT * PORTRAIT, f) == PORTRAIT * PORTRAIT && fgetc(f) == EOF;
    if (f) fclose(f);
    if (ok) {
        /* XRGB words (B,G,R,0 bytes) of the pinned conversion: opaque. */
        for (unsigned i = 0; i < PORTRAIT * PORTRAIT; i++) portrait[i] = 0xff000000u | (portrait[i] & 0xffffffu);
        area(portrait, PORTRAIT, icon->pixels, size);
        icon->icon.size = size; icon->icon.argb = icon->pixels;
        S.ciuki[slot] = &icon->icon;
    } else {
        free(icon);
        S.ciuki[slot] = &unavailable;
    }
    free(portrait);
    return ok ? S.ciuki[slot] : NULL;
}

const struct du_icon *du_icon(const char *tango_name, unsigned size)
{
    int slot = slot_of(size);
    if (!tango_name || slot < 0) return NULL;
    if (!strcmp(tango_name, DU_ICON_CIUKI)) return ciuki(slot);
    if (open_store()) return NULL;
    int n = find(tango_name);
    if (n < 0) return NULL;
    const struct du_icon **cell = &S.cache[(unsigned)n * 4 + (unsigned)slot];
    if (*cell) return *cell == &unavailable ? NULL : *cell;
    unsigned k = le16(name_entry((unsigned)n) + 8 + 2 * (unsigned)slot);
    struct cached *icon = k < S.images ? malloc(sizeof(*icon) + (size_t)size * size * 4) : NULL;
    if (!icon || decode(k, icon->pixels, size, 0)) {
        free(icon);
        *cell = &unavailable;
        return NULL;
    }
    icon->icon.size = size; icon->icon.argb = icon->pixels;
    return *cell = &icon->icon;
}

const uint32_t *du_icon_argb(const char *tango_name, unsigned size)
{
    const struct du_icon *icon = du_icon(tango_name, size);
    return icon ? icon->argb : NULL;
}

unsigned du_icon_count(void) { return open_store() ? 0 : S.names; }

const char *du_icon_store_name(unsigned index)
{ return open_store() || index >= S.names ? NULL : string_at(le32(name_entry(index))); }

const char *du_icon_notice(void) { return open_store() ? NULL : string_at(S.notice); }
