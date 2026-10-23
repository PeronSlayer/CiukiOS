/* Software reference engine; original fbdev integer conversion paths.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/swdib.h>
struct pixel_layout {
    unsigned bytes, red_shift, red_pos, green_shift, green_pos, blue_shift, blue_pos;
};

static struct pixel_layout pixel_layout(const struct fb_device *d)
{
    return (struct pixel_layout){ d->bpp / 8, 8 - d->red_size, d->red_pos,
                                 8 - d->green_size, d->green_pos, 8 - d->blue_size, d->blue_pos };
}

static uint32_t pack_layout(const struct pixel_layout *l, uint32_t rgb)
{
    return ((((rgb >> 16) & 255u) >> l->red_shift) << l->red_pos) |
           ((((rgb >> 8) & 255u) >> l->green_shift) << l->green_pos) |
           (((rgb & 255u) >> l->blue_shift) << l->blue_pos);
}

uint32_t swdib_pack(const struct fb_device *d, uint32_t rgb)
{
    const struct pixel_layout l = pixel_layout(d);
    return pack_layout(&l, rgb);
}

/* Unaligned RAM loads and generic integer stores are valid on i686. The
 * lowered C alignment and alias permission also cover arbitrary row pitch. */
typedef uint32_t fb_word __attribute__((aligned(1), may_alias));
typedef uint16_t fb_half __attribute__((aligned(1), may_alias));

static void pixel(uint8_t *p, unsigned bytes, uint32_t colour)
{
    if (bytes == 2)
        *(fb_half *)p = (uint16_t)colour;
    else if (bytes == 4)
        *(fb_word *)p = colour;
    else {
        p[0] = (uint8_t)colour;
        p[1] = (uint8_t)(colour >> 8);
        p[2] = (uint8_t)(colour >> 16);
    }
}

/* Intel SDM Vol. 3A, 253668-084US, sections 12.3/12.3.1 and 9.2.5:
 * https://cdrdv2-public.intel.com/825758/253668-sdm-vol-3a.pdf
 * Consecutive WC writes combine; partially filled buffers drain at the next
 * serializing event. A compiler barrier per row preserves generated stores,
 * without volatile per pixel or a hardware fence per row. Keep the existing
 * framebuffer_write_drain() before ownership release and the end timestamp. */
static void row_written(void) { __asm__ volatile("" ::: "memory"); }

/* Both pointers must be dword aligned; count is in dwords, not bytes. */
static void row_copy(uint8_t *dst, const void *src, size_t count)
{
    __asm__ volatile("cld\n\trep movsl" : "+D"(dst), "+S"(src), "+c"(count) : : "memory", "cc");
}

static void row_fill(uint8_t *dst, uint32_t colour, size_t count)
{
    __asm__ volatile("cld\n\trep stosl" : "+D"(dst), "+c"(count) : "a"(colour) : "memory", "cc");
}

static void present_xrgb_row(uint8_t *dst, const uint8_t *src, unsigned width)
{
    uint32_t used = 0;
    for (unsigned x = 0; x < width; x++) used |= ((const fb_word *)src)[x];
    if (!(used & 0xff000000u)) {
        row_copy(dst, src, width);
        return;
    }
    /* X is ignored by the public surface contract. Normalize in RAM, never
     * by reading or masking a destination word in the WC framebuffer. */
    uint32_t pixels[64];
    while (width) {
        unsigned n = width > ARRAY_SIZE(pixels) ? ARRAY_SIZE(pixels) : width;
        for (unsigned x = 0; x < n; x++) pixels[x] = ((const fb_word *)src)[x] & 0xffffffu;
        row_copy(dst, pixels, n);
        src += n * 4; dst += n * 4; width -= n;
    }
}

static uint16_t pack_565(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 8) & 0xf800u) | ((rgb >> 5) & 0x07e0u) | ((rgb >> 3) & 0x001fu));
}

static void present_565_row(uint8_t *dst, const uint8_t *src, unsigned width)
{
    if ((uintptr_t)dst & 2u) {
        pixel(dst, 2, pack_565(*(const fb_word *)src));
        dst += 2; src += 4; width--;
    }
    uint16_t pixels[64] __attribute__((aligned(4)));
    while (width >= 2) {
        unsigned n = width > ARRAY_SIZE(pixels) ? ARRAY_SIZE(pixels) : width & ~1u;
        for (unsigned x = 0; x < n; x++) pixels[x] = pack_565(((const fb_word *)src)[x]);
        row_copy(dst, pixels, n / 2);
        src += n * 4; dst += n * 2; width -= n;
    }
    if (width) pixel(dst, 2, pack_565(*(const fb_word *)src));
}

void swdib_present(const struct fb_device *d, const struct fb_surface *s, const struct fb_rect *r, const struct fb_rect *c)
{
    uint32_t sx = (uint32_t)((int64_t)c->x - r->x), sy = (uint32_t)((int64_t)c->y - r->y);
    const struct pixel_layout l = pixel_layout(d);
    bool xrgb = l.bytes == 4 && !l.red_shift && l.red_pos == 16 &&
                !l.green_shift && l.green_pos == 8 && !l.blue_shift && !l.blue_pos;
    bool rgb565 = l.bytes == 2 && l.red_shift == 3 && l.red_pos == 11 &&
                  l.green_shift == 2 && l.green_pos == 5 && l.blue_shift == 3 && !l.blue_pos;
    for (int32_t y = 0; y < c->height; y++) {
        const uint8_t *sp = (const uint8_t *)s->pixels + (sy + (uint32_t)y) * s->pitch + sx * 4;
        uint8_t *dp = d->mapped + (c->y + (uint32_t)y) * d->pitch + (uint32_t)c->x * l.bytes;
        if (xrgb && !(((uintptr_t)sp | (uintptr_t)dp) & 3u))
            present_xrgb_row(dp, sp, (unsigned)c->width);
        else if (rgb565 && !((uintptr_t)dp & 1u))
            present_565_row(dp, sp, (unsigned)c->width);
        else for (int32_t x = 0; x < c->width; x++)
            pixel(dp + (uint32_t)x * l.bytes, l.bytes, pack_layout(&l, ((const fb_word *)sp)[x]));
        row_written();
    }
}

void swdib_fill(const struct fb_device *d, const struct fb_rect *c, uint32_t colour)
{
    unsigned bytes = d->bpp / 8;
    for (int32_t y = 0; y < c->height; y++) {
        uint8_t *dp = d->mapped + (c->y + (uint32_t)y) * d->pitch + (uint32_t)c->x * bytes;
        unsigned width = (unsigned)c->width;
        if (bytes == 4 && !((uintptr_t)dp & 3u))
            row_fill(dp, colour, width);
        else if (bytes == 2 && !((uintptr_t)dp & 1u)) {
            if ((uintptr_t)dp & 2u) { pixel(dp, 2, colour); dp += 2; width--; }
            row_fill(dp, colour | (colour << 16), width / 2);
            if (width & 1u) pixel(dp + (width & ~1u) * 2, 2, colour);
        } else for (int32_t x = 0; x < c->width; x++)
            pixel(dp + (uint32_t)x * bytes, bytes, colour);
        row_written();
    }
}

void swdib_copy(const struct fb_device *d, const struct fb_rect *r, int32_t x, int32_t y)
{
    unsigned bytes = d->bpp / 8;
    for (int32_t i = 0; i < r->height; i++) {
        int32_t row = y > r->y ? r->height - 1 - i : i;
        uint8_t *dst = d->mapped + (uint32_t)(y + row)*d->pitch + (uint32_t)x*bytes;
        const uint8_t *src = d->mapped + (uint32_t)(r->y + row)*d->pitch + (uint32_t)r->x*bytes;
        size_t n = (size_t)r->width*bytes;
        if (y == r->y && x > r->x) while (n) { n--; dst[n] = src[n]; }
        else for (size_t j = 0; j < n; j++) dst[j] = src[j];
        row_written();
    }
}
void swdib_pattern(const struct fb_device *d, const struct fb_rect *c, const uint8_t pattern[8], uint32_t fg, uint32_t bg)
{
    unsigned bytes = d->bpp / 8;
    for (int32_t y = c->y; y < c->y + c->height; y++) {
        for (int32_t x = c->x; x < c->x + c->width; x++)
            pixel(d->mapped + (uint32_t)y*d->pitch + (uint32_t)x*bytes, bytes,
                  pattern[y & 7] & (0x80u >> (x & 7)) ? fg : bg);
        row_written();
    }
}
void swdib_convert(const struct fb_device *d, const struct fb_rect *dst, void *rows, uint32_t stride)
{
    const struct pixel_layout l = pixel_layout(d);
    for (int32_t y = 0; y < dst->height; y++) {
        uint8_t *row = (uint8_t *)rows + (uint32_t)y * stride;
        for (int32_t x = 0; x < dst->width; x++) {
            uint32_t v = pack_layout(&l, ((const fb_word *)row)[x]);
            pixel(row + (uint32_t)x*l.bytes, l.bytes, v);
        }
    }
}
void swdib_write(const struct fb_device *d, const struct fb_rect *dst, const struct fb_rect *c, const void *rows, uint32_t stride)
{
    unsigned bytes = d->bpp / 8;
    for (int32_t y = c->y; y < c->y + c->height; y++) {
        const uint8_t *src = (const uint8_t *)rows + (uint32_t)(y - dst->y)*stride + (uint32_t)(c->x - dst->x)*bytes;
        uint8_t *dp = d->mapped + (uint32_t)y*d->pitch + (uint32_t)c->x*bytes;
        if (bytes == 4 && !(((uintptr_t)dp | (uintptr_t)src) & 3u)) row_copy(dp, src, (size_t)c->width);
        else for (uint32_t i = 0; i < (uint32_t)c->width*bytes; i++) dp[i] = src[i];
        row_written();
    }
}
