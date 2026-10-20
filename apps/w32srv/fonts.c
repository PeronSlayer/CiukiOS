/* f5-10C2: the native side of the PE-to-native font bridge. w32srv opens the
 * store faces once for every Win32 process and rasterises glyphs with the
 * pinned SDK libfont (FreeType 2.13.3, TrueType interpreter v35, grey
 * coverage); gdi32 caches the processed coverage per process. Requests name a
 * face by its native store path; nothing is mapped into the Win32 process.
 * Glyph tops are converted to the Win32 orientation here (GLYPHMETRICS
 * gmptGlyphOrigin.y, FreeType bitmap_top: above the baseline is positive).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fonts.h"
#include <ciuki/font.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define WF_FILE_MAX (16u << 20)
#define WF_FACE_ALLOWANCE (64u << 10)
#define WF_SDK_CACHE (64u << 10)
struct wf_face {
    char path[WF_DIR + WF_NAME + 2];
    struct ciuki_font_face *face;
    uint32_t file_bytes, resident, glyphs, last, refused, installed;
    long long stamp; /* install store: size and modification time seen */
};
struct wf_service {
    char system_dir[WF_DIR], install_dir[WF_DIR];
    struct wf_face faces[WF_FACES];
    uint32_t clock;
    struct w32r_font_stats stats;
};

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int prefix(const char *path, const char *dir)
{
    size_t n = strlen(dir);
    for (size_t i = 0; i < n; i++)
        if (lower((unsigned char)path[i]) != lower((unsigned char)dir[i])) return 0;
    return path[n] == '/';
}
/* A face is a plain file name directly inside one of the two stores. */
static int store_member(const struct wf_service *s, const char *path, uint32_t *installed)
{
    const char *base = NULL;
    *installed = 0;
    if (prefix(path, s->system_dir)) base = path + strlen(s->system_dir) + 1;
    else if (prefix(path, s->install_dir)) base = path + strlen(s->install_dir) + 1, *installed = 1;
    if (!base) return 0;
    size_t n = strlen(base);
    if (n < 5 || n > WF_NAME || base[0] == '.') return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)base[i];
        if (c < 32 || c == 127 || c == '/' || c == '\\' || c == ':') return 0;
    }
    const char *ext = base + n - 4;
    return ext[0] == '.' && lower(ext[1]) == 't' && lower(ext[2]) == 't' && lower(ext[3]) == 'f';
}
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
/* Table-directory validation before FreeType sees an installed file
 * (OpenType "Organization of an OpenType Font", table directory and table
 * records; TrueType outlines need glyf/loca). Returns a Win32 error. */
static uint32_t validate(const char *path, uint32_t *file_bytes)
{
    FILE *f = fopen(path, "rb");
    if (!f) return errno == ENOENT ? 2 : 5;
    uint8_t header[12 + 16 * 64];
    long size = fseek(f, 0, SEEK_END) ? -1 : ftell(f);
    size_t got = size > 0 && !fseek(f, 0, SEEK_SET) ? fread(header, 1, sizeof(header), f) : 0;
    uint32_t error = 0;
    if (size < 12 || (unsigned long)size > WF_FILE_MAX || got < 12) { error = 13; goto out; }
    uint32_t tag = be32(header);
    unsigned tables = be16(header + 4);
    /* 'OTTO' (CFF), 'ttcf' (collections) and others: not TrueType sfnt. */
    if ((tag != 0x00010000u && tag != 0x74727565u) || !tables || tables > 64 ||
        got < 12 + 16u * tables) { error = 13; goto out; }
    static const char required[][5] = {"head", "hhea", "maxp", "hmtx", "cmap", "glyf", "loca"};
    unsigned found = 0;
    uint32_t head_at = 0;
    for (unsigned i = 0; i < tables; i++) {
        const uint8_t *r = header + 12 + 16 * i;
        uint32_t at = be32(r + 8), length = be32(r + 12);
        if (at > (uint32_t)size || length > (uint32_t)size - at) { error = 13; goto out; }
        for (unsigned k = 0; k < 7; k++)
            if (!memcmp(r, required[k], 4)) {
                found |= 1u << k;
                if (!k) head_at = length >= 54 ? at : 0;
            }
    }
    uint8_t head[54];
    if (found != 0x7fu || !head_at || fseek(f, (long)head_at, SEEK_SET) ||
        fread(head, 1, sizeof(head), f) != sizeof(head) || be32(head + 12) != 0x5f0f3cf5u ||
        be16(head + 18) < 16 || be16(head + 18) > 16384) { error = 13; goto out; }
    *file_bytes = (uint32_t)size;
out:
    fclose(f);
    return error;
}
static uint32_t resident(const struct wf_face *f)
{
    struct ciuki_font_cache_stats c = {0};
    if (f->face) ciuki_font_cache_info(f->face, &c);
    return f->file_bytes + WF_FACE_ALLOWANCE + (uint32_t)c.bytes;
}
static void account(struct wf_service *s)
{
    uint32_t bytes = 0, open = 0;
    for (unsigned i = 0; i < WF_FACES; i++)
        if (s->faces[i].face) { bytes += s->faces[i].resident; open++; }
    s->stats.face_bytes = bytes;
    s->stats.open_faces = open;
    if (bytes > s->stats.face_peak) s->stats.face_peak = bytes;
}
static void close_face(struct wf_service *s, struct wf_face *f)
{
    if (f->face) { ciuki_font_close(f->face); f->face = NULL; f->resident = 0; s->stats.evictions++; }
}
/* Opens (or finds) a store face within the shared budget; least recently
 * used faces close first. Refusals are remembered per path. */
static uint32_t face_for(struct wf_service *s, const char *path, struct wf_face **out)
{
    uint32_t installed;
    if (!store_member(s, path, &installed)) return 5;
    struct wf_face *slot = NULL, *lru = NULL;
    for (unsigned i = 0; i < WF_FACES; i++) {
        struct wf_face *f = &s->faces[i];
        if (f->path[0] && !strcmp(f->path, path)) { slot = f; break; }
        if (!f->path[0] && !lru) lru = f;
    }
    if (!slot) {
        if (!lru) /* forget the least recently used closed or open entry */
            for (unsigned i = 0; i < WF_FACES; i++)
                if (!lru || s->faces[i].last < lru->last) lru = &s->faces[i];
        close_face(s, lru);
        memset(lru, 0, sizeof(*lru));
        snprintf(lru->path, sizeof(lru->path), "%s", path);
        slot = lru;
    }
    slot->last = ++s->clock;
    /* The system store is immutable; a file in the install store can be
     * removed and replaced under the same name (RemoveFontResource then
     * AddFontResource), so its size and time are compared on every use. */
    if (installed) {
        struct stat st;
        long long stamp = stat(path, &st) ? -1 : (long long)st.st_size * 1000003LL + (long long)st.st_mtime;
        if (slot->stamp && stamp != slot->stamp) { close_face(s, slot); slot->refused = 0; }
        slot->stamp = stamp;
        slot->installed = 1;
    }
    if (slot->refused) return slot->refused;
    if (slot->face) { *out = slot; return 0; }
    uint32_t error = validate(path, &slot->file_bytes);
    uint32_t need = slot->file_bytes + 2 * WF_FACE_ALLOWANCE;
    if (!error && need > s->stats.budget) error = 8;
    if (error) {
        if (error != 2 && error != 5) slot->refused = error;
        s->stats.refusals++;
        return error;
    }
    for (;;) {
        account(s);
        if (s->stats.face_bytes + need <= s->stats.budget) break;
        struct wf_face *victim = NULL;
        for (unsigned i = 0; i < WF_FACES; i++)
            if (s->faces[i].face && (!victim || s->faces[i].last < victim->last)) victim = &s->faces[i];
        if (!victim) break;
        close_face(s, victim);
    }
    struct ciuki_font_metrics m;
    if (ciuki_font_open(path, &slot->face)) {
        slot->face = NULL;
        s->stats.refusals++;
        if (errno == ENOMEM) return 8;
        slot->refused = 13;
        return 13;
    }
    if (ciuki_font_get_metrics(slot->face, &m)) {
        ciuki_font_close(slot->face); slot->face = NULL; slot->refused = 13; s->stats.refusals++;
        return 13;
    }
    slot->glyphs = m.glyph_count;
    slot->resident = resident(slot);
    s->stats.opens++;
    account(s);
    *out = slot;
    return 0;
}
/* The selection layer keeps the processed coverage; trim the SDK face cache
 * so the accounted resident bytes stay file + 128 KiB at most. */
static void trim(struct wf_service *s, struct wf_face *f)
{
    struct ciuki_font_cache_stats c;
    if (!ciuki_font_cache_info(f->face, &c) && c.bytes > WF_SDK_CACHE) ciuki_font_cache_clear(f->face);
    f->resident = resident(f);
    account(s);
}

struct wf_service *wf_create(const struct wf_config *c)
{
    if (!c || !c->system_dir || !c->install_dir || strlen(c->system_dir) >= WF_DIR || strlen(c->install_dir) >= WF_DIR)
        return NULL;
    struct wf_service *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    snprintf(s->system_dir, sizeof(s->system_dir), "%s", c->system_dir);
    snprintf(s->install_dir, sizeof(s->install_dir), "%s", c->install_dir);
    s->stats.version = 1;
    s->stats.faces = WF_FACES;
    s->stats.budget = c->budget ? c->budget : WF_BUDGET;
    return s;
}
void wf_destroy(struct wf_service *s)
{
    if (!s) return;
    for (unsigned i = 0; i < WF_FACES; i++) if (s->faces[i].face) ciuki_font_close(s->faces[i].face);
    free(s);
}
void wf_statistics(struct wf_service *s, struct w32r_font_stats *out) { *out = s->stats; }

uint32_t wf_call(void *opaque, uint32_t pid, const struct w32r_request *a, const char *path,
                 const void *data, struct w32r_reply *r, uint8_t *out, size_t cap)
{
    struct wf_service *s = opaque;
    (void)pid;
    if (a->key || a->access || a->flags) return 87;
    s->stats.requests++;
    if (a->op == W32R_FONT_STATS) {
        if (*path || a->index || a->type || a->data_bytes || cap < sizeof(s->stats)) return 87;
        memcpy(out, &s->stats, sizeof(s->stats));
        r->data_bytes = sizeof(s->stats);
        return 0;
    }
    if (a->op == W32R_FONT_CMAP) {
        if (a->type || a->data_bytes || (a->index & 255u) || a->index >= 0x110000u || cap < 512) return 87;
    } else if (a->op == W32R_FONT_GLYPHS) {
        if (!a->type || a->type > W32R_FONT_BATCH || a->data_bytes != 4u * a->type) return 87;
        if (a->index < 64 || a->index > 128u * 64u) { s->stats.refusals++; return 50; }
    } else return 87;
    struct wf_face *f = NULL;
    uint32_t error = face_for(s, path, &f);
    if (error) return error;
    r->key = f->resident;
    if (a->op == W32R_FONT_CMAP) {
        for (unsigned i = 0; i < 256; i++) {
            unsigned glyph = 0;
            if (ciuki_font_lookup(f->face, a->index + i, &glyph) || glyph > 0xffffu) glyph = 0;
            out[2 * i] = (uint8_t)glyph;
            out[2 * i + 1] = (uint8_t)(glyph >> 8);
        }
        r->data_bytes = 512;
        r->count = 256;
        s->stats.cmap_pages++;
        return 0;
    }
    const uint8_t *ids = data;
    for (unsigned i = 0; i < a->type; i++) {
        uint32_t glyph;
        memcpy(&glyph, ids + 4 * i, 4);
        if (glyph >= f->glyphs) return 87;
    }
    size_t at = 0;
    unsigned done = 0;
    for (; done < a->type; done++) {
        uint32_t glyph;
        memcpy(&glyph, ids + 4 * done, 4);
        struct ciuki_font_bitmap b;
        if (ciuki_font_raster(f->face, glyph, a->index, &b)) {
            error = errno == ENOMEM ? 8 : 13;
            break;
        }
        size_t bytes = (size_t)b.width * b.height;
        int fits = b.width <= 0xffffu && b.height <= 0xffffu && b.left >= -32768 && b.left <= 32767 &&
                   b.top >= -32767 && b.top <= 32768 && at + sizeof(struct w32r_glyph) + bytes <= cap &&
                   at + sizeof(struct w32r_glyph) + bytes <= W32R_DATA;
        if (fits) {
            struct w32r_glyph g = {glyph, (uint16_t)b.width, (uint16_t)b.height, (int16_t)b.left,
                                   (int16_t)-b.top, b.advance};
            memcpy(out + at, &g, sizeof(g));
            for (unsigned y = 0; y < b.height; y++)
                memcpy(out + at + sizeof(g) + (size_t)y * b.width, b.coverage + (size_t)y * b.stride, b.width);
            at += sizeof(g) + bytes;
        }
        ciuki_font_bitmap_free(&b);
        if (!fits) break;
    }
    trim(s, f);
    r->key = f->resident;
    if (!done) return error ? error : 8;
    s->stats.glyphs += done;
    r->count = done;
    r->data_bytes = (uint32_t)at;
    return 0;
}
