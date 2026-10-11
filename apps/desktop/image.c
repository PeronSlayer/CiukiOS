/* SPDX-License-Identifier: MIT
 * Wallpaper image decoding: PNG, JPEG and BMP through the vendored
 * stb_image 2.30 (third_party/stb, MIT/Public Domain). Memory in, ARGB out. */
#include "wallpaper.h"
#include <stdlib.h>
#include <string.h>
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_FAILURE_STRINGS
#define STBI_ASSERT(x) ((void)0)
#define STBI_MAX_DIMENSIONS DESK_IMAGE_MAX_SIDE
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wall"
#pragma clang diagnostic ignored "-Wextra"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wdouble-promotion"
#pragma clang diagnostic ignored "-Wframe-larger-than"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include "../../third_party/stb/stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

int desk_image_decode(const uint8_t *data, size_t size, struct desk_image *out)
{
    memset(out, 0, sizeof(*out));
    if (!data || size < 8 || size > 0x7fffffff) return -1;
    int w = 0, h = 0, n = 0;
    if (!stbi_info_from_memory(data, (int)size, &w, &h, &n) || w <= 0 || h <= 0 ||
        w > DESK_IMAGE_MAX_SIDE || h > DESK_IMAGE_MAX_SIDE || (uint64_t)w * (uint64_t)h > DESK_IMAGE_MAX_PIXELS) return -1;
    unsigned char *rgba = stbi_load_from_memory(data, (int)size, &w, &h, &n, 4);
    if (!rgba) return -1;
    uint32_t *argb = malloc((size_t)w * (size_t)h * 4u);
    if (!argb) { stbi_image_free(rgba); return -1; }
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
        const unsigned char *p = rgba + i * 4;
        argb[i] = (uint32_t)p[3] << 24 | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    }
    stbi_image_free(rgba);
    out->pixels = argb; out->width = (uint32_t)w; out->height = (uint32_t)h;
    return 0;
}

void desk_image_free(struct desk_image *image)
{
    free(image->pixels);
    memset(image, 0, sizeof(*image));
}
