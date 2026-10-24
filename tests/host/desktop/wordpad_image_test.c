/* WordPad picture codec host tests (G28b): stb_image decoding of PNG, JPEG, GIF and DIB, the reduction of big pictures, PNG encoding.
 *   wordpad_image_test <folder with small.png big.png tiny.dib [small.jpg small.gif]>
 * SPDX-License-Identifier: MIT */
#include "image.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *folder, const char *name, size_t *n)
{
    char path[1024]; snprintf(path, sizeof(path), "%s/%s", folder, name);
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)len); assert(b && fread(b, 1, (size_t)len, f) == (size_t)len); fclose(f);
    *n = (size_t)len; return b;
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    size_t n; uint32_t *px; unsigned w, h;
    uint8_t *png = slurp(argv[1], "small.png", &n); assert(png);
    assert(!wp_image_decode(WP_PIC_PNG, png, n, &px, &w, &h) && w == 8 && h == 4 && px[0] == 0x0a141e && px[31] == 0x0a141e); free(px);
    assert(!wp_image_decode(0, png, n, &px, &w, &h) && w == 8); free(px);                    /* an image file: sniffed */
    assert(wp_image_decode(WP_PIC_WMF, png, n, &px, &w, &h) == -ENOTSUP && wp_image_decode(WP_PIC_EMF, png, n, &px, &w, &h) == -ENOTSUP);
    assert(wp_image_decode(WP_PIC_PNG, (const uint8_t *)"not an image at all", 19, &px, &w, &h) == -EINVAL && !px);
    assert(wp_image_decode(WP_PIC_PNG, png, 0, &px, &w, &h) == -EINVAL && wp_image_decode(WP_PIC_PNG, png, 20, &px, &w, &h) == -EINVAL);   /* truncated */
    /* a header that promises more than the limit */
    uint8_t huge[64]; memcpy(huge, png, 33); huge[16] = 0; huge[17] = 1; huge[18] = 0; huge[19] = 0; huge[22] = 0; huge[23] = 1; huge[24] = 0; huge[25] = 0;
    assert(wp_image_decode(WP_PIC_PNG, huge, 33, &px, &w, &h) < 0);
    /* a DIB (no file header) */
    size_t dn; uint8_t *dib = slurp(argv[1], "tiny.dib", &dn); assert(dib);
    assert(!wp_image_decode(WP_PIC_DIB, dib, dn, &px, &w, &h) && w == 4 && h == 2 && px[0] == 0x0a141e && px[7] == 0x0a141e); free(px);
    assert(wp_image_decode(WP_PIC_DIB, dib, 10, &px, &w, &h) == -EINVAL);
    free(dib);
    /* a big picture keeps its memory bounded by a whole-factor reduction */
    size_t bn; uint8_t *big = slurp(argv[1], "big.png", &bn); assert(big);
    assert(!wp_image_decode(WP_PIC_PNG, big, bn, &px, &w, &h) && w == 1500 && h == 1000 && (uint64_t)w * h <= WP_IMAGE_KEEP && px[0] == 0xff0000 && px[(size_t)w * h - 1] == 0xff0000); free(px);
    free(big);
    /* optional formats */
    size_t jn; uint8_t *jpg = slurp(argv[1], "small.jpg", &jn);
    if (jpg) { assert(!wp_image_decode(WP_PIC_JPEG, jpg, jn, &px, &w, &h) && w == 16 && h == 8); unsigned g = px[0] >> 8 & 255; assert(g > 150); free(px); free(jpg); }
    uint8_t *gif = slurp(argv[1], "small.gif", &jn);
    if (gif) { assert(!wp_image_decode(0, gif, jn, &px, &w, &h) && w == 16 && h == 8); free(px); free(gif); }
    /* encoding and reading back */
    uint32_t pixels[6] = {0xff0000, 0x00ff00, 0x0000ff, 0xffffff, 0x000000, 0x123456};
    uint8_t *out; size_t on;
    assert(!wp_image_png(pixels, 3, 2, &out, &on) && !memcmp(out, "\x89PNG", 4));
    assert(!wp_image_decode(WP_PIC_PNG, out, on, &px, &w, &h) && w == 3 && h == 2 && !memcmp(px, pixels, sizeof(pixels))); free(px); free(out);
    assert(wp_image_png(pixels, 0, 2, &out, &on) == -EINVAL);
    free(png);
    puts("PASS wordpad image");
    return 0;
}
