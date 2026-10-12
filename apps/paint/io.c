/* SPDX-License-Identifier: MIT
 * CiukPaint's original BMP8 format plus stb PNG/BMP24. Files are replaced
 * only after all writes and close succeed; decode builds a separate image. */
#include "io.h"
#include "../../third_party/stb/stb_image.h"
#include "../../third_party/stb/stb_image_write.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int dimensions(int w, int h)
{ return w > 0 && h > 0 && w <= PAINT_MAX_AXIS && h <= PAINT_MAX_AXIS && (unsigned)w * (unsigned)h <= PAINT_MAX_PIXELS; }
static uint8_t dac8(uint8_t v) { return (uint8_t)((v << 2) | (v >> 4)); }
static void put16(uint8_t *p, unsigned n) { p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8); }
static void put32(uint8_t *p, uint32_t n) { put16(p, n); put16(p + 2, n >> 16); }
static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static uint8_t nearest(const uint8_t *pal, int r, int g, int b)
{
    unsigned best = 65535; int bi = 16;
    for (int pass = 0; pass < 2; pass++) for (int i = pass ? 0 : 16; i < (pass ? 16 : PAINT_COLORS); i++) {
        int dr = pal[i * 3] - r, dg = pal[i * 3 + 1] - g, db = pal[i * 3 + 2] - b;
        unsigned d = (unsigned)(2 * dr * dr + 4 * dg * dg + 3 * db * db);
        if ((!pass && d < best) || (pass && (d * 2 < best || !d))) {
            bi = i; best = pass ? d / 2 : d; if (!d) return (uint8_t)i;
        }
    }
    return (uint8_t)bi;
}

int paint_read_image(const char *path, const uint8_t *pal, struct paint_image *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -errno;
    int r = -EINVAL, w, h, comp;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return -EIO; }
    long length = ftell(f);
    if (length <= 0 || length > 32 * 1024 * 1024) { fclose(f); return -EFBIG; }
    rewind(f);
    uint8_t *data = malloc((size_t)length);
    if (!data) { fclose(f); return -ENOMEM; }
    size_t read = fread(data, 1, (size_t)length, f);
    int failed = ferror(f); if (fclose(f)) failed = 1;
    if (failed || read != (size_t)length) { free(data); return -EIO; }
    if (!stbi_info_from_memory(data, (int)length, &w, &h, &comp)) goto done;
    /* stb's BMP info reports the signed DIB height, while its decoder
     * normalizes top-down images. Normalize before the allocation gate. */
    if (data[0] == 'B' && data[1] == 'M' && h < 0) {
        if (h < -PAINT_MAX_AXIS) { r = -EFBIG; goto done; }
        h = -h;
    }
    if (!dimensions(w, h)) { r = -EFBIG; goto done; }
    if (length >= 54 && data[0] == 'B' && data[1] == 'M' && get32(data + 14) >= 40) {
        unsigned bits = data[28] | (unsigned)data[29] << 8;
        uint64_t stride = (((uint64_t)(unsigned)w * bits + 31) / 32) * 4;
        /* stb's memory stream substitutes zeroes for missing bytes. Reject
         * a truncated bitmap instead of turning unread pixels black. */
        if ((uint64_t)get32(data + 10) + stride * (unsigned)h > (uint64_t)length) goto done;
    }
    uint8_t *rgba = stbi_load_from_memory(data, (int)length, &w, &h, &comp, 4);
    if (!rgba) goto done;
    size_t n = (size_t)w * (size_t)h;
    uint8_t *pixels = malloc(n);
    if (!pixels) { stbi_image_free(rgba); r = -ENOMEM; goto done; }
    /* Cache exact six-bit RGB keys. The old five-bit cache merged distinct
     * palette colours; six bits preserve exported RGB on every round trip. */
    uint8_t *cache = malloc(1u << 18);
    if (cache) memset(cache, 255, 1u << 18);
    int bitmap = length >= 2 && data[0] == 'B' && data[1] == 'M';
    for (size_t i = 0; i < n; i++) {
        unsigned a = bitmap ? 255 : rgba[i * 4 + 3]; /* legacy BMP ignores alpha */
        int red = (int)((rgba[i * 4] * a + 255 * (255 - a) + 127) / 255);
        int green = (int)((rgba[i * 4 + 1] * a + 255 * (255 - a) + 127) / 255);
        int blue = (int)((rgba[i * 4 + 2] * a + 255 * (255 - a) + 127) / 255);
        unsigned key = (unsigned)(red >> 2) << 12 | (unsigned)(green >> 2) << 6 | (unsigned)(blue >> 2);
        uint8_t c = cache ? cache[key] : 255;
        if (c == 255) { c = nearest(pal, red >> 2, green >> 2, blue >> 2); if (cache) cache[key] = c; }
        pixels[i] = c;
    }
    free(cache);
    stbi_image_free(rgba);
    *out = (struct paint_image){w, h, pixels}; r = 0;
done:
    free(data); return r;
}

struct writer { FILE *file; int failed; };
static void write_bytes(void *arg, void *data, int n)
{
    struct writer *w = arg;
    if (n < 0 || (!w->failed && fwrite(data, 1, (size_t)n, w->file) != (size_t)n)) w->failed = 1;
}
static int write_bmp8(struct writer *wr, const struct paint_image *im, const uint8_t *pal)
{
    uint8_t used[PAINT_COLORS] = {0}, map[PAINT_COLORS] = {0}, header[54] = {0};
    size_t n = (size_t)im->w * (size_t)im->h;
    for (size_t i = 0; i < n; i++) {
        if (im->pixels[i] >= PAINT_COLORS) return -EINVAL;
        used[im->pixels[i]] = 1;
    }
    unsigned colors = 0;
    for (int i = 0; i < PAINT_COLORS; i++) if (used[i]) map[i] = (uint8_t)colors++;
    unsigned stride = ((unsigned)im->w + 3) & ~3u, off = 54 + colors * 4;
    header[0] = 'B'; header[1] = 'M';
    put32(header + 2, off + stride * (unsigned)im->h); put32(header + 10, off);
    put32(header + 14, 40); put32(header + 18, (unsigned)im->w); put32(header + 22, (unsigned)im->h);
    put16(header + 26, 1); put16(header + 28, 8); put32(header + 34, stride * (unsigned)im->h);
    put32(header + 38, 2835); put32(header + 42, 2835); put32(header + 46, colors); put32(header + 50, colors);
    write_bytes(wr, header, sizeof(header));
    for (int i = 0; i < PAINT_COLORS; i++) if (used[i]) {
        uint8_t entry[4] = {dac8(pal[i * 3 + 2]), dac8(pal[i * 3 + 1]), dac8(pal[i * 3]), 0};
        write_bytes(wr, entry, sizeof(entry));
    }
    uint8_t *row = calloc(stride, 1);
    if (!row) return -ENOMEM;
    for (int y = im->h - 1; y >= 0 && !wr->failed; y--) {
        for (int x = 0; x < im->w; x++) row[x] = map[im->pixels[(size_t)y * (size_t)im->w + (size_t)x]];
        write_bytes(wr, row, (int)stride);
    }
    free(row); return wr->failed ? -EIO : 0;
}

int paint_write_image(const char *path, enum paint_format format, const struct paint_image *im, const uint8_t *pal)
{
    if (!dimensions(im->w, im->h) || !im->pixels || format < PAINT_BMP8 || format > PAINT_PNG) return -EINVAL;
    char temp[PAINT_PATH_MAX]; static unsigned serial;
    int fd = -1;
    for (int i = 0; i < 32; i++) {
        int n = snprintf(temp, sizeof(temp), "%s.paint-%u.tmp", path, ++serial);
        if (n < 0 || (size_t)n >= sizeof(temp)) return -ENAMETOOLONG;
        fd = open(temp, O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (fd >= 0) break;
        if (errno != EEXIST) return -errno;
    }
    if (fd < 0) return -EEXIST;
    FILE *f = fdopen(fd, "wb");
    if (!f) { int r = -errno; close(fd); unlink(temp); return r; }
    struct writer wr = {f, 0}; int r = 0;
    if (format == PAINT_BMP8) r = write_bmp8(&wr, im, pal);
    else {
        size_t n = (size_t)im->w * (size_t)im->h;
        uint8_t *rgb = malloc(n * 3);
        if (!rgb) r = -ENOMEM;
        else {
            for (size_t i = 0; i < n; i++) {
                unsigned c = im->pixels[i]; if (c >= PAINT_COLORS) { r = -EINVAL; break; }
                for (int k = 0; k < 3; k++) rgb[i * 3 + (unsigned)k] = dac8(pal[c * 3 + (unsigned)k]);
            }
            if (!r && !(format == PAINT_PNG ? stbi_write_png_to_func(write_bytes, &wr, im->w, im->h, 3, rgb, im->w * 3) :
                                            stbi_write_bmp_to_func(write_bytes, &wr, im->w, im->h, 3, rgb))) r = -ENOMEM;
            free(rgb);
        }
    }
    if (wr.failed || fflush(f)) r = -EIO;
    if (fclose(f)) r = -EIO;
    if (!r && rename(temp, path)) r = -errno;
    if (r) unlink(temp);
    return r;
}
