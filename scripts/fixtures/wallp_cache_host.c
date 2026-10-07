/* Host harness: compiles the production wallp.c into this translation unit and
 * supplies only its DOS, XMS, and compositor boundaries. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
struct host { int screen_w, screen_h; } host_state;
#define HOST host_state
#define CIUKIOS_APP_H
struct app_wallpaper_info {
    unsigned short bytes, generation;
    unsigned char index, kind;
    char filename[13];
    unsigned char style;
};
#define WP_FILL 0
#define WP_FIT 1
#define WP_STRETCH 2
#define WP_CENTER 3
#define WP_TILE 4
#define C_TEAL 3
#define EV_OPEN 1
#define EV_PAINT 2
#define EV_POLL 6
#define EV_CLOSE 7
#define EV_SUSPEND 8

int app_band_info(void *out);
void far_copy(u16 dseg, u16 doff, u16 sseg, u16 soff, u16 n);
u16 app_seg(void);
void app_log(const char *, const char *);
void ui_rect(int, int, int, int, int);
int dos_close(int);
int dos_open(const char *, int);
long dos_seek(int, long, int);
int dos_read(int, void *, u16);
void mem_copy(void *, const void *, u16);
int mem_cmp(const void *, const void *, u16);
void mem_set(void *, int, u16);
unsigned str_len(const char *);
void str_copy(char *, const char *);
void str_cat(char *, const char *);
int str_cmp(const char *, const char *);

#include "../../src/apps/wallp.c"

#define HANDLE_MAX 16
static struct { u8 *p; size_t n; } blocks[HANDLE_MAX];
static u8 band_mem[4 * 2560 * 1600];
static struct band test_band;
static int bad_transfer, render_rowbytes;
u8 webstore_pixels[6146];

u16 webstore_alloc(u32 n)
{
    u16 h;
    for (h = 1; h < HANDLE_MAX; ++h) if (!blocks[h].p) break;
    assert(h < HANDLE_MAX);
    blocks[h].p = calloc(1, n);
    blocks[h].n = n;
    return blocks[h].p ? h : 0;
}
void webstore_free(u16 h)
{
    if (h && blocks[h].p) { free(blocks[h].p); blocks[h].p = 0; blocks[h].n = 0; }
}
int webstore_read(u16 h, u32 off, void *dst, u16 n)
{
    if (!h || !blocks[h].p || ((off | n) & 1) || off + n > blocks[h].n) {
        bad_transfer = 1; return 0;
    }
    memcpy(dst, blocks[h].p + off, n); return 1;
}
int webstore_write(u16 h, u32 off, const void *src, u16 n)
{
    if (!h || !blocks[h].p || ((off | n) & 1) || off + n > blocks[h].n) {
        bad_transfer = 1; return 0;
    }
    memcpy(blocks[h].p + off, src, n); return 1;
}
int app_band_info(void *out) { memcpy(out, &test_band, sizeof test_band); return 1; }
u16 app_seg(void) { return 0x1000; }
void far_copy(u16 ds, u16 doff, u16 ss, u16 soff, u16 n)
{
    uintptr_t base = (uintptr_t)native_row;
    unsigned k;
    if (ds != test_band.seg || ss != app_seg() || doff + n > sizeof band_mem) {
        bad_transfer = 1; return;
    }
    for (k = 0; k < sizeof native_row; ++k) {
        if ((u16)(base + k) == soff) {
            memcpy(band_mem + (size_t)(test_band.top - 29) * render_rowbytes + doff,
                   native_row + k, n);
            return;
        }
    }
    bad_transfer = 1;
}
void ui_rect(int x, int y, int w, int h, int c)
{ (void)x; (void)y; (void)w; (void)h; (void)c; }
void app_log(const char *a, const char *b) { (void)a; (void)b; }
int dos_close(int h) { (void)h; return 0; }
int dos_open(const char *p, int m) { (void)p; (void)m; return -1; }
long dos_seek(int h, long p, int m) { (void)h; (void)p; (void)m; return -1; }
int dos_read(int h, void *p, u16 n) { (void)h; (void)p; (void)n; return -1; }
void mem_copy(void *d, const void *s, u16 n) { memcpy(d, s, n); }
int mem_cmp(const void *a, const void *b, u16 n) { return memcmp(a, b, n); }
void mem_set(void *d, int v, u16 n) { memset(d, v, n); }
unsigned str_len(const char *s) { return (unsigned)strlen(s); }
void str_copy(char *d, const char *s) { strcpy(d, s); }
void str_cat(char *d, const char *s) { strcat(d, s); }
int str_cmp(const char *a, const char *b) { return strcmp(a, b); }

static u8 channel(int x, int y, int c)
{ return (u8)(c == 0 ? x * 17 + y * 3 : c == 1 ? x * 5 + y * 31 : x * 11 + y * 7); }
static void fail(const char *what, int w, int h, int style, int bytes, int x, int y)
{
    fprintf(stderr, "%s mode=%dx%d style=%d bytes=%d at=%d,%d\n",
            what, w, h, style, bytes, x, y);
    exit(1);
}

int main(void)
{
    static const int modes[][2] = {
        {640, 480}, {800, 600}, {1024, 768}, {1280, 800}, {1600, 900}, {2560, 1600}
    };
    static const int formats[][7] = {
        {2, 5, 11, 6, 5, 5, 0}, {3, 8, 16, 8, 8, 8, 0},
        {4, 8, 16, 8, 8, 8, 0}
    };
    static const int source_sizes[][2] = {
        {1672, 941}, {320, 200}, {321, 199}, {1, 256}, {256, 1}
    };
    int mi, fi, st, si, total_cases = 0;
    for (mi = 0; mi < 6; ++mi) for (fi = 0; fi < 3; ++fi)
    for (st = 0; st < 5; ++st) for (si = 0; si < 5; ++si) {
        if (mi < 5 && si >= 3) continue;
        if (mi == 5 && !(fi == 2 && (st == WP_FILL || st == WP_FIT) && si >= 3)) continue;
        int sw = source_sizes[si][0], sh = source_sizes[si][1];
        int w = modes[mi][0], h = modes[mi][1], avail = h - 61;
        int dw = sw, dh = sh, x, y, left, top, cw, chh, bw = w - 7, bleft = 3;
        int bytes = formats[fi][0], rowbytes, rs = formats[fi][1], rp = formats[fi][2];
        int gs = formats[fi][3], gp = formats[fi][4], bs = formats[fi][5], bp = formats[fi][6];
        int i, band_height;
        u32 source_bytes;
        u16 shandle;

        if (st == WP_FILL || st == WP_FIT) {
            int by_width = (u32)w * sh >= (u32)avail * sw;
            if (st == WP_FIT) by_width = !by_width;
            if (by_width) {
                dw = w; dh = (sh * w + (st == WP_FILL ? sw - 1 : 0)) / sw;
            } else {
                dh = avail; dw = (sw * avail + (st == WP_FILL ? sh - 1 : 0)) / sh;
            }
            if (!dw) dw = 1;
            if (!dh) dh = 1;
        } else if (st == WP_STRETCH || st == WP_TILE) { dw = w; dh = avail; }
        x = (w - dw) / 2; y = 29 + (avail - dh) / 2;
        left = x > 0 ? x : 0; top = y > 29 ? y : 29;
        cw = dw + (x < 0 ? x : 0); chh = dh + (y < 29 ? y - 29 : 0);
        if (cw > w - left) cw = w - left;
        if (chh > 29 + avail - top) chh = 29 + avail - top;

        style = st; width = (u16)sw; height = (u16)sh; stride = (u16)((sw * 3 + 1) & ~1);
        HOST.screen_w = w; HOST.screen_h = h;
        format = (struct band){0, 0, 0, 0, 0, (u8)bytes, (u8)rs, (u8)rp,
                               (u8)gs, (u8)gp, (u8)bs, (u8)bp, 0};
        format_known = 1; format_w = w; format_h = h; dirty = 1;
        active = cache = prepared = staging = 0;
        source_bytes = (u32)stride * sh;
        shandle = webstore_alloc(source_bytes); assert(shandle); active = shandle;
        for (y = 0; y < sh; ++y) for (x = 0; x < sw; ++x) {
            u8 *p = blocks[active].p + (u32)y * stride + x * 3;
            p[0] = channel(x, y, 0); p[1] = channel(x, y, 1); p[2] = channel(x, y, 2);
        }
        assert(prepare_begin());
        for (i = 0; prepared && i < 200; ++i) prepare_step();
        assert(!prepared && cache && !bad_transfer);

        /* Check every prepared pixel against independent nearest-neighbour
         * geometry and RGB mask packing. */
        for (y = 0; y < chh; ++y) for (x = 0; x < cw; ++x) {
            int px = left + x, py = top + y, sx, sy;
            u8 *got = blocks[cache].p + (u32)y * cache_stride + x * bytes;
            u32 expected;
            if (st == WP_TILE) { sx = px % sw; sy = (py - 29) % sh; }
            else {
                sx = (int)((u32)(px - (w - dw) / 2) * sw / dw);
                sy = (int)((u32)(py - (29 + (avail - dh) / 2)) * sh / dh);
            }
            if (sx < 0) sx = 0; if (sx >= sw) sx = sw - 1;
            if (sy < 0) sy = 0; if (sy >= sh) sy = sh - 1;
            if (bytes == 4 && rs == 8 && rp == 16 && gs == 8 && gp == 8 && bs == 8 && bp == 0) {
                if (got[0] != channel(sx, sy, 2) || got[1] != channel(sx, sy, 1) ||
                    got[2] != channel(sx, sy, 0) || got[3])
                    fail("prepared BGR", w, h, st, bytes, px, py);
            } else {
                expected = ((u32)(channel(sx, sy, 0) >> (8 - rs)) << rp) |
                            ((u32)(channel(sx, sy, 1) >> (8 - gs)) << gp) |
                            ((u32)(channel(sx, sy, 2) >> (8 - bs)) << bp);
                if (got[0] != (u8)expected || got[1] != (u8)(expected >> 8) ||
                    (bytes >= 3 && got[2] != (u8)(expected >> 16)) ||
                    (bytes == 4 && got[3]))
                    fail("prepared pixel", w, h, st, bytes, px, py);
            }
        }

        memset(band_mem, 0xA5, sizeof band_mem);
        rowbytes = (bw * bytes + 1) & ~1; rowbytes += 4; render_rowbytes = rowbytes;
        test_band = (struct band){0x2000, 29, (u16)(h - 32), (u16)rowbytes,
                                  (u16)bw, (u8)bytes, (u8)rs, (u8)rp,
                                  (u8)gs, (u8)gp, (u8)bs, (u8)bp, (u16)bleft};
        band_height = 60000 / rowbytes;
        for (y = 29; y < h - 32; y += band_height) {
            test_band.top = (u16)y;
            test_band.bottom = (u16)(y + band_height < h - 32 ? y + band_height : h - 32);
            paint();
        }
        for (y = 29; y < h - 32; ++y) for (x = 0; x < 2560; ++x) {
            size_t at = (size_t)(y - 29) * rowbytes + (size_t)(x - bleft) * bytes;
            int inside = x >= bleft && x < bleft + bw && x >= left && x < left + cw &&
                         y >= top && y < top + chh;
            u8 *want = inside ? blocks[cache].p + (u32)(y - top) * cache_stride +
                                (x - left) * bytes : 0;
            if (inside && memcmp(band_mem + at, want, bytes))
                fail("paint mismatch", w, h, st, bytes, x, y);
            if (!inside && x >= bleft && x < bleft + bw && band_mem[at] != 0xA5)
                fail("paint escaped shape", w, h, st, bytes, x, y);
        }
        assert(!bad_transfer);
        webstore_free(active); webstore_free(cache); active = cache = 0; ++total_cases;
    }
    printf("PASS production wallp.c prepare/paint; cases=%d; XMS alignment/bounds and band canaries held\n",
           total_cases);
    return 0;
}
