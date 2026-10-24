/* SPDX-License-Identifier: MIT
 * WordPad picture decoding and encoding on stb: see image.h. Unmodified upstream headers (third_party/stb/README.md). */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS   /* the SDK excludes compiler TLS */
#define STBI_MAX_DIMENSIONS 8192
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
/* stb_image keeps its GIF state and PNG palette buffers on the stack: the decoder runs at a shallow depth on the
 * 1 MiB main thread stack, as the Image Viewer's (see apps/accessories/viewer_stb.c). */
#pragma clang diagnostic ignored "-Wframe-larger-than"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wdouble-promotion"
#pragma clang diagnostic ignored "-Wcast-qual"
#include "../../third_party/stb/stb_image.h"
#include "../../third_party/stb/stb_image_write.h"
#include "image.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

int wp_image_decode(unsigned kind, const uint8_t *data, size_t len, uint32_t **px, unsigned *w, unsigned *h)
{
    *px = NULL; *w = *h = 0;
    if (kind == WP_PIC_WMF || kind == WP_PIC_EMF || kind == WP_PIC_PICT) return -ENOTSUP;
    if (!data || !len || len > 0x7fffffffu) return -EINVAL;
    uint8_t *bmp = NULL;
    if (kind == WP_PIC_DIB) {
        size_t bl;
        bmp = wp_dib_to_bmp(data, len, &bl);
        if (!bmp) return -EINVAL;
        data = bmp; len = bl;
    }
    int iw, ih, n, e = 0;
    uint32_t *out = NULL;
    unsigned char *rgba = NULL;
    if (!stbi_info_from_memory(data, (int)len, &iw, &ih, &n)) { e = -EINVAL; goto done; }
    if (iw <= 0 || ih <= 0 || iw > WP_IMAGE_SIDE || ih > WP_IMAGE_SIDE || (uint64_t)iw * (uint64_t)ih > WP_IMAGE_PIXELS) { e = -EFBIG; goto done; }
    rgba = stbi_load_from_memory(data, (int)len, &iw, &ih, &n, 4);
    if (!rgba) { e = -EINVAL; goto done; }
    out = malloc((size_t)iw * (size_t)ih * sizeof(*out));
    if (!out) { e = -ENOMEM; goto done; }
    for (size_t i = 0; i < (size_t)iw * (size_t)ih; i++) {
        unsigned r = rgba[4 * i], g = rgba[4 * i + 1], b = rgba[4 * i + 2], a = rgba[4 * i + 3];
        r = (r * a + 255 * (255 - a) + 127) / 255; g = (g * a + 255 * (255 - a) + 127) / 255; b = (b * a + 255 * (255 - a) + 127) / 255;
        out[i] = r << 16 | g << 8 | b;
    }
    if ((uint64_t)iw * (uint64_t)ih > WP_IMAGE_KEEP) {   /* keep the memory of a big picture bounded: whole-factor reduction */
        unsigned step = 2;
        while ((uint64_t)(iw / step) * (uint64_t)(ih / step) > WP_IMAGE_KEEP) step++;
        unsigned nw = (unsigned)iw / step, nh = (unsigned)ih / step;
        uint32_t *small = malloc((size_t)nw * nh * sizeof(*small));
        if (small) {
            for (unsigned y = 0; y < nh; y++) for (unsigned x = 0; x < nw; x++) small[(size_t)y * nw + x] = out[(size_t)(y * step) * (unsigned)iw + x * step];
            free(out); out = small; iw = (int)nw; ih = (int)nh;
        }
    }
    *px = out; *w = (unsigned)iw; *h = (unsigned)ih;
done:
    if (rgba) stbi_image_free(rgba);
    free(bmp);
    return e;
}

struct sink { uint8_t *p; size_t n, cap; int err; };
static void collect(void *ctx, void *data, int size)
{
    struct sink *s = ctx;
    if (s->err || size < 0) return;
    if (s->n + (size_t)size > s->cap) {
        size_t c = s->cap ? s->cap : 4096;
        while (c < s->n + (size_t)size) c *= 2;
        uint8_t *q = realloc(s->p, c);
        if (!q) { s->err = -ENOMEM; return; }
        s->p = q; s->cap = c;
    }
    memcpy(s->p + s->n, data, (size_t)size); s->n += (size_t)size;
}
int wp_image_png(const uint32_t *px, unsigned w, unsigned h, uint8_t **out, size_t *len)
{
    if (!w || !h || w > WP_IMAGE_SIDE || h > WP_IMAGE_SIDE) return -EINVAL;
    uint8_t *rgb = malloc((size_t)w * h * 3);
    if (!rgb) return -ENOMEM;
    for (size_t i = 0; i < (size_t)w * h; i++) { rgb[3 * i] = (uint8_t)(px[i] >> 16); rgb[3 * i + 1] = (uint8_t)(px[i] >> 8); rgb[3 * i + 2] = (uint8_t)px[i]; }
    struct sink s = {0};
    int ok = stbi_write_png_to_func(collect, &s, (int)w, (int)h, 3, rgb, (int)(w * 3));
    free(rgb);
    if (!ok || s.err) { free(s.p); return s.err ? s.err : -ENOMEM; }
    *out = s.p; *len = s.n;
    return 0;
}
