/* SPDX-License-Identifier: MIT
 * Image Viewer decoding and integer scaling. */
#include "viewer.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "../../third_party/stb/stb_image.h"

static const char *extension(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && dot != name ? dot + 1 : "";
}
int viewer_is_image_name(const char *name)
{
    static const char *const known[] = { "png", "jpg", "jpeg", "jpe", "bmp", "gif", "xrgb" };
    const char *e = extension(name);
    for (unsigned i = 0; i < sizeof(known) / sizeof(*known); i++) if (!strcasecmp(e, known[i])) return 1;
    return 0;
}
void viewer_free(struct viewer_image *im) { free(im->pixels); memset(im, 0, sizeof(*im)); }

int viewer_decode(const uint8_t *data, size_t size, const char *name, struct viewer_image *out)
{
    memset(out, 0, sizeof(*out));
    if (!data || !size) return -EINVAL;
    if (!strcasecmp(extension(name), "xrgb")) {
        /* Raw XRGB8888 (B,G,R,0) squares: the portrait and cached wallpapers. */
        size_t pixels = size / 4, side = 0;
        while ((side + 1) * (side + 1) <= pixels) side++;
        if (size % 4 || !side || side * side != pixels || side > VIEWER_MAX_SIDE) return -EINVAL;
        out->pixels = malloc(size);
        if (!out->pixels) return -ENOMEM;
        for (size_t i = 0; i < pixels; i++) {
            uint32_t v; memcpy(&v, data + 4 * i, 4); out->pixels[i] = v & 0xffffffu;
        }
        out->width = out->height = (int)side; strcpy(out->format, "XRGB");
        return 0;
    }
    if (size > VIEWER_MAX_FILE || size > 0x7fffffffu) return -EFBIG;
    int w, h, n;
    if (!stbi_info_from_memory(data, (int)size, &w, &h, &n)) return -EINVAL;
    if (w <= 0 || h <= 0 || w > VIEWER_MAX_SIDE || h > VIEWER_MAX_SIDE || (uint64_t)w * (uint64_t)h > VIEWER_MAX_PIXELS) return -EFBIG;
    unsigned char *rgba = stbi_load_from_memory(data, (int)size, &w, &h, &n, 4);
    if (!rgba) return -EINVAL;
    out->pixels = malloc((size_t)w * (size_t)h * 4);
    if (!out->pixels) { stbi_image_free(rgba); return -ENOMEM; }
    /* Composite transparency over the classic light grey checker colour. */
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
        unsigned r = rgba[4 * i], g = rgba[4 * i + 1], b = rgba[4 * i + 2], a = rgba[4 * i + 3];
        unsigned bg = 0xc0;
        r = (r * a + bg * (255 - a) + 127) / 255; g = (g * a + bg * (255 - a) + 127) / 255; b = (b * a + bg * (255 - a) + 127) / 255;
        out->pixels[i] = (r << 16) | (g << 8) | b;
    }
    stbi_image_free(rgba);
    out->width = w; out->height = h;
    static const uint8_t png[4] = { 0x89, 'P', 'N', 'G' };
    strcpy(out->format, size >= 4 && !memcmp(data, png, 4) ? "PNG" : size >= 2 && data[0] == 0xff && data[1] == 0xd8 ? "JPEG" :
                        size >= 2 && data[0] == 'B' && data[1] == 'M' ? "BMP" : size >= 3 && !memcmp(data, "GIF", 3) ? "GIF" : "IMAGE");
    return 0;
}

static const int steps[] = { 10, 25, 33, 50, 67, 100, 150, 200, 300, 400, 800 };
#define STEPS (int)(sizeof(steps) / sizeof(*steps))
int viewer_zoom_step(int zoom, int direction)
{
    if (direction > 0) { for (int i = 0; i < STEPS; i++) if (steps[i] > zoom) return steps[i]; return steps[STEPS - 1]; }
    for (int i = STEPS - 1; i >= 0; i--) if (steps[i] < zoom) return steps[i];
    return steps[0];
}
int viewer_fit_percent(const struct viewer_image *im, int w, int h)
{
    if (!im->width || !im->height || w <= 0 || h <= 0) return 100;
    int zx = (int)((int64_t)w * 100 / im->width), zy = (int)((int64_t)h * 100 / im->height);
    int z = zx < zy ? zx : zy;
    if (z > 100) z = 100;    /* best fit never enlarges, as Windows' viewer */
    return z < 1 ? 1 : z;
}
void viewer_blit(struct du_canvas *c, struct du_rect area, const struct viewer_image *im, int zoom, int sx, int sy)
{
    if (!im->pixels || zoom <= 0) return;
    int64_t dw = (int64_t)im->width * zoom / 100, dh = (int64_t)im->height * zoom / 100;
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    /* Centre images smaller than the area; otherwise start at the scroll offset. */
    int ox = dw < area.w ? area.x + (int)(area.w - dw) / 2 : area.x - (int)((int64_t)sx * zoom / 100);
    int oy = dh < area.h ? area.y + (int)(area.h - dh) / 2 : area.y - (int)((int64_t)sy * zoom / 100);
    struct du_rect target = du_intersection(du_intersection(area, c->clip), (struct du_rect){ox, oy, (int)dw, (int)dh});
    for (int y = target.y; y < target.y + target.h; y++) {
        int iy = (int)((int64_t)(y - oy) * im->height / dh);
        const uint32_t *row = im->pixels + (size_t)iy * (size_t)im->width;
        uint32_t *dst = c->pixels + (size_t)y * (size_t)c->width;
        for (int x = target.x; x < target.x + target.w; x++) dst[x] = row[(int64_t)(x - ox) * im->width / dw];
    }
}
