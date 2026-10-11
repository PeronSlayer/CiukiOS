/* SPDX-License-Identifier: MIT
 * Wallpaper engine (wallpaper.h). Original procedural designs, integer-only,
 * deterministic for a given resolution; no third-party artwork. */
#include "wallpaper.h"
#include "settings.h"
#include <stdlib.h>
#include <string.h>

extern uint8_t desk_glyph_row(unsigned, unsigned);

const struct desk_builtin_wallpaper desk_builtin_wallpapers[DESK_BUILTIN_WALLPAPERS] = {
    {"dawn", "Platinum Dawn"}, {"pinstripe", "Platinum Pinstripe"}, {"deepblue", "Ciuki Blue"},
    {"twilight", "Twilight"}, {"linen", "Linen"}, {"blueprint", "Blueprint"}};

static const int8_t bayer[4][4] = {{-8, 0, -6, 2}, {4, -4, 6, -2}, {-5, 3, -7, 1}, {7, -1, 5, -3}};

static uint32_t channel_clamp(int v) { return v < 0 ? 0u : v > 255 ? 255u : (uint32_t)v; }

/* c0..c1 at t/4096, plus an ordered-dither offset in 1/16 levels. */
static uint32_t lerp(uint32_t c0, uint32_t c1, int t, int dither)
{
    if (t < 0) t = 0;
    if (t > 4096) t = 4096;
    uint32_t out = 0;
    for (int shift = 0; shift < 24; shift += 8) {
        int a = (int)((c0 >> shift) & 255), b = (int)((c1 >> shift) & 255);
        int v = (a * 16 * (4096 - t) + b * 16 * t) / 4096 + dither;   /* 1/16 level units */
        out |= channel_clamp((v + 8) / 16) << shift;
    }
    return out;
}

static uint32_t shade(uint32_t c, int delta)
{
    uint32_t out = 0;
    for (int shift = 0; shift < 24; shift += 8) out |= channel_clamp((int)((c >> shift) & 255) + delta) << shift;
    return out;
}

static uint32_t hash(uint32_t x, uint32_t y)
{
    uint32_t h = x * 0x9e3779b1u ^ y * 0x85ebca77u;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
    return h;
}

void desk_builtin_paint(const char *id, uint32_t *out, uint32_t width, uint32_t height, uint32_t stride)
{
    int w = (int)width, h = (int)height;
    if (w <= 0 || h <= 0) return;
    for (int y = 0; y < h; y++) {
        uint32_t *row = out + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            int d = bayer[y & 3][x & 3];
            uint32_t c;
            if (!strcmp(id, "pinstripe")) {
                /* Platinum pinstripes with a soft vertical light falloff. */
                int t = y * 4096 / (h > 1 ? h - 1 : 1);
                c = lerp(0xe4e8ecu, 0xc9d0d6u, t, d);
                if (y & 1) c = shade(c, -7);
            } else if (!strcmp(id, "deepblue")) {
                int dx = x - w / 2, dy = y - h * 2 / 5;
                int64_t r = (int64_t)dx * dx + (int64_t)dy * dy * 2;
                int64_t m = (int64_t)w * w / 4 + (int64_t)h * h / 2;
                c = lerp(0x4a7898u, 0x152836u, (int)(r * 4096 / (m ? m : 1)), d);
            } else if (!strcmp(id, "twilight")) {
                int t = (x * 3 + y * 5) * 4096 / (w * 3 + h * 5);
                c = t < 2048 ? lerp(0x274a5eu, 0x4f5f80u, t * 2, d) : lerp(0x4f5f80u, 0x7a6585u, (t - 2048) * 2, d);
            } else if (!strcmp(id, "linen")) {
                int n = (int)(hash((uint32_t)x, (uint32_t)y) & 15) - 8;
                int weave = ((x & 3) == 0 ? -5 : 0) + ((y & 3) == 0 ? -4 : 0);
                c = shade(0xd9d3c6u, n / 2 + weave);
            } else if (!strcmp(id, "blueprint")) {
                int t = y * 4096 / (h > 1 ? h - 1 : 1);
                c = lerp(0x2c5570u, 0x1f3f56u, t, d);
                if (x % 48 == 0 || y % 48 == 0) c = shade(c, 22);
                else if (x % 12 == 0 && y % 12 == 0) c = shade(c, 16);
            } else {   /* "dawn" and unknown ids */
                int t = y * 4096 / (h > 1 ? h - 1 : 1);
                c = t < 2600 ? lerp(0xd2dbe2u, 0x9fb2c0u, t * 4096 / 2600, d) : lerp(0x9fb2c0u, 0x6f879au, (t - 2600) * 4096 / 1496, d);
                if (((x + y) % 7) == 0) c = shade(c, 3);
            }
            row[x] = c;
        }
    }
}

static uint32_t over(uint32_t argb, uint32_t back)
{
    unsigned a = argb >> 24;
    if (a == 255) return argb & 0xffffffu;
    uint32_t out = 0;
    for (unsigned shift = 0; shift < 24; shift += 8)
        out |= ((((argb >> shift) & 255) * a + ((back >> shift) & 255) * (255 - a) + 127) / 255) << shift;
    return out;
}

/* Area average (downscale) or bilinear (upscale) of src into dst w*h. */
static void resample(const struct desk_image *s, uint32_t *dst, uint32_t w, uint32_t h, uint32_t stride, uint32_t colour)
{
    if (!w || !h || !s->width || !s->height) return;
    int up = w >= s->width && h >= s->height;
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        uint32_t px;
        if (up) {
            /* Centre-aligned bilinear in 8.8 fixed point. */
            int64_t fx = ((int64_t)(2 * x + 1) * s->width * 128) / w - 128;
            int64_t fy = ((int64_t)(2 * y + 1) * s->height * 128) / h - 128;
            if (fx < 0) fx = 0;
            if (fy < 0) fy = 0;
            uint32_t x0 = (uint32_t)(fx >> 8), y0 = (uint32_t)(fy >> 8);
            uint32_t ax = (uint32_t)(fx & 255), ay = (uint32_t)(fy & 255);
            uint32_t x1 = x0 + 1 < s->width ? x0 + 1 : x0, y1 = y0 + 1 < s->height ? y0 + 1 : y0;
            if (x0 >= s->width) x0 = x1 = s->width - 1;
            if (y0 >= s->height) y0 = y1 = s->height - 1;
            uint32_t p00 = s->pixels[(size_t)y0 * s->width + x0], p10 = s->pixels[(size_t)y0 * s->width + x1];
            uint32_t p01 = s->pixels[(size_t)y1 * s->width + x0], p11 = s->pixels[(size_t)y1 * s->width + x1];
            px = 0;
            for (unsigned shift = 0; shift < 32; shift += 8) {
                uint32_t a = (p00 >> shift) & 255, b = (p10 >> shift) & 255, c = (p01 >> shift) & 255, e = (p11 >> shift) & 255;
                uint32_t top = a * (256 - ax) + b * ax, bot = c * (256 - ax) + e * ax;
                px |= ((top * (256 - ay) + bot * ay + 32768) >> 16) << shift;
            }
        } else {
            uint32_t sx0 = (uint32_t)((uint64_t)x * s->width / w), sx1 = (uint32_t)((uint64_t)(x + 1) * s->width / w);
            uint32_t sy0 = (uint32_t)((uint64_t)y * s->height / h), sy1 = (uint32_t)((uint64_t)(y + 1) * s->height / h);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sy1 <= sy0) sy1 = sy0 + 1;
            uint64_t sum[4] = {0, 0, 0, 0}, n = 0;
            for (uint32_t yy = sy0; yy < sy1 && yy < s->height; yy++)
                for (uint32_t xx = sx0; xx < sx1 && xx < s->width; xx++) {
                    uint32_t p = s->pixels[(size_t)yy * s->width + xx];
                    for (unsigned k = 0; k < 4; k++) sum[k] += (p >> (k * 8)) & 255;
                    n++;
                }
            px = 0;
            if (n) for (unsigned k = 0; k < 4; k++) px |= (uint32_t)((sum[k] + n / 2) / n) << (k * 8);
        }
        dst[(size_t)y * stride + x] = over(px, colour) | (px & 0xff000000u);
    }
}

void desk_image_scale(const struct desk_image *src, uint32_t *dst, uint32_t w, uint32_t h, int mode, uint32_t colour)
{
    for (size_t i = 0; i < (size_t)w * h; i++) dst[i] = colour;
    if (!src || !src->pixels || !w || !h) return;
    if (mode == CFG_WALL_TILE) {
        for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++)
            dst[(size_t)y * w + x] = over(src->pixels[(size_t)(y % src->height) * src->width + x % src->width], colour);
        return;
    }
    if (mode == CFG_WALL_CENTER) {
        int32_t ox = ((int32_t)w - (int32_t)src->width) / 2, oy = ((int32_t)h - (int32_t)src->height) / 2;
        for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
            int32_t sx = (int32_t)x - ox, sy = (int32_t)y - oy;
            if (sx >= 0 && sy >= 0 && sx < (int32_t)src->width && sy < (int32_t)src->height)
                dst[(size_t)y * w + x] = over(src->pixels[(size_t)sy * src->width + (uint32_t)sx], colour);
        }
        return;
    }
    uint32_t tw = w, th = h, ox = 0, oy = 0;
    if (mode == CFG_WALL_FIT) {
        if ((uint64_t)src->width * h > (uint64_t)src->height * w) th = (uint32_t)((uint64_t)src->height * w / src->width);
        else tw = (uint32_t)((uint64_t)src->width * h / src->height);
        if (!tw) tw = 1;
        if (!th) th = 1;
        ox = (w - tw) / 2; oy = (h - th) / 2;
    }
    resample(src, dst + (size_t)oy * w + ox, tw, th, w, colour);
    for (uint32_t y = oy; y < oy + th; y++) for (uint32_t x = ox; x < ox + tw; x++) dst[(size_t)y * w + x] &= 0xffffffu;
}

static void caption(uint32_t *out, uint32_t width, uint32_t height, int32_t x, int32_t y, const char *s, uint32_t colour)
{
    /* The compositor's 5x7 glyphs at scale 2, as the plain desktop draws "CiukiOS". */
    for (; *s; s++, x += 12) for (unsigned row = 0; row < 7; row++) {
        uint8_t bits = desk_glyph_row((unsigned char)*s, row);
        for (unsigned col = 0; col < 5; col++) if (bits & (1u << (4 - col)))
            for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) {
                int32_t px = x + (int32_t)col * 2 + dx, py = y + (int32_t)row * 2 + dy;
                if (px >= 0 && py >= 0 && (uint32_t)px < width && (uint32_t)py < height) out[(size_t)py * width + (uint32_t)px] = colour;
            }
    }
}

int desk_wallpaper_render(uint32_t *out, uint32_t width, uint32_t height, uint32_t top, uint32_t bottom,
                          const char *spec, int mode, uint32_t colour,
                          const struct desk_image *image, const uint32_t *portrait)
{
    colour &= 0xffffffu;
    for (size_t i = 0; i < (size_t)width * height; i++) out[i] = colour;
    if (!spec || !strcmp(spec, "none")) return 0;
    if (!strcmp(spec, "ciuki")) {
        /* Exactly where the plain compositor puts the approved portrait. */
        if (!portrait || width < 256) return portrait ? 0 : -1;
        int32_t x0 = ((int32_t)width - 256) / 2;
        int32_t y0 = (int32_t)top + ((int32_t)height - (int32_t)top - 256 - 36) / 2;
        if (y0 < (int32_t)top) y0 = (int32_t)top;
        for (int32_t y = 0; y < 256; y++) {
            if (y0 + y < 0 || (uint32_t)(y0 + y) >= height) continue;
            for (int32_t x = 0; x < 256; x++) {
                uint32_t p = portrait[y * 256 + x] & 0xffffffu;
                /* The converted array is composited over #375564; its fully
                 * transparent surround takes the chosen desktop colour. */
                out[(size_t)(y0 + y) * width + (uint32_t)(x0 + x)] = p == 0x375564u ? colour : p;
            }
        }
        caption(out, width, height, ((int32_t)width - 84) / 2, y0 + 256 + 10, "CiukiOS", 0xf6dfb6u);
        return 0;
    }
    uint32_t area_h = height > top + bottom ? height - top - bottom : height;
    uint32_t oy = height > top + bottom ? top : 0;
    if (!strncmp(spec, "builtin:", 8)) {
        desk_builtin_paint(spec + 8, out, width, height, width);
        return 0;
    }
    if (!image || !image->pixels) return -1;
    if (mode == CFG_WALL_TILE) {
        desk_image_scale(image, out, width, height, mode, colour);
        return 0;
    }
    /* Centre, stretch and fit use the area between the top bar and the taskbar. */
    uint32_t *box = malloc((size_t)width * area_h * 4u);
    if (!box) return -1;
    desk_image_scale(image, box, width, area_h, mode, colour);
    memcpy(out + (size_t)oy * width, box, (size_t)width * area_h * 4u);
    free(box);
    return 0;
}
