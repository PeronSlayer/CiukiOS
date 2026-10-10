/* F1 framebuffer fixtures and photographed live colour bars.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/mm.h>
#include <ciuki/probe.h>
#include <ciuki/fbdev.h>

#define FW 17u
#define FH 13u
#define SW 25u
#define SH 21u
#define SP (SW * 4 + 7)
#define GUARD 32u
#define SENTINEL 0xA5

static const struct fb_rect rectangles[] = {
    { 0, 0, FW, FH }, { 3, 2, 9, 7 }, { 6, 4, 8, 6 },
    { -4, 3, 10, 5 }, { 13, 3, 10, 5 }, { 2, -3, 7, 8 }, { 2, 10, 7, 8 },
    { -3, -2, 25, 21 }, { 4, 5, 0, 4 }, { 4, 5, 4, 0 },
    { -25, 0, 3, 3 }, { 20, 0, 3, 3 }, { 0, -25, 3, 3 }, { 0, 20, 3, 3 },
    { 0, 0, 1, 1 }, { FW - 1, 0, 1, 1 }, { 0, FH - 1, 1, 1 },
    { FW - 1, FH - 1, 1, 1 },
};

static uint32_t source_colour(unsigned x, unsigned y)
{
    return 0xD3000000u | (((x * 37 + y * 11) & 255) << 16) |
           (((x * 13 + y * 47) & 255) << 8) | ((x * 71 + y * 3) & 255);
}

/* Independent fixture oracle: visit screen pixels and select the last
 * operation covering each coordinate. It neither clips rectangles nor
 * shares presenter helpers. The standalone reference renderer requested
 * by f1-05 lives only in tests/host/fbdev_test.c. */
static void expected_pixels(const struct fb_device *d, uint8_t *out, unsigned last)
{
    for (unsigned y = 0; y < FH; y++) {
        for (unsigned x = 0; x < FW; x++) {
            uint32_t rgb = 0;
            bool found = false;
            for (unsigned i = 0; i <= last; i++) {
                const struct fb_rect *r = &rectangles[i];
                int64_t sx = (int64_t)x - r->x, sy = (int64_t)y - r->y;
                if (sx >= 0 && sy >= 0 && sx < r->width && sy < r->height) {
                    rgb = i & 1 ? 0x619BD7u + i : source_colour((unsigned)sx, (unsigned)sy);
                    found = true;
                }
            }
            if (found) {
                uint32_t packed = (((rgb >> 16) & 255) << d->red_pos) |
                                  (((rgb >> 8) & 255) << d->green_pos) |
                                  ((rgb & 255) << d->blue_pos);
                for (unsigned b = 0; b < d->bpp / 8; b++)
                    out[y * d->pitch + x * (d->bpp / 8) + b] = (uint8_t)(packed >> (8 * b));
            }
        }
    }
}

static int fixtures(uint32_t *digest, unsigned *guards, unsigned *errors)
{
    uint8_t *source = kmalloc(SP * SH);
    if (!source)
        return -ENOMEM;
    memset(source, SENTINEL, SP * SH);
    for (unsigned y = 0; y < SH; y++)
        for (unsigned x = 0; x < SW; x++) {
            uint32_t rgb = source_colour(x, y);
            memcpy(source + y * SP + x * 4, &rgb, 4);
        }
    struct fb_surface s = { source, SW, SH, SP, SP * SH };
    for (unsigned bpp = 24; bpp <= 32; bpp += 8) {
        for (unsigned pad = 0; pad <= 7; pad += 7) {
            uint32_t pitch = FW * (bpp / 8) + pad, size = pitch * FH;
            uint8_t *raw = kmalloc(size + 2 * GUARD), *expected = kmalloc(size);
            if (!raw || !expected) {
                kfree(raw);
                kfree(expected);
                kfree(source);
                return -ENOMEM;
            }
            memset(raw, SENTINEL, size + 2 * GUARD);
            memset(expected, SENTINEL, size);
            struct fb_device d = {
                .width = FW, .height = FH, .pitch = pitch, .size = size,
                .mapped = raw + GUARD, .bpp = bpp, .present = true,
                .red_size = 8, .red_pos = 16, .green_size = 8, .green_pos = 8,
                .blue_size = 8, .blue_pos = 0, .rsvd_size = bpp == 32 ? 8 : 0, .rsvd_pos = 24,
                .handle = -1,
            };
            for (unsigned i = 0; i < ARRAY_SIZE(rectangles); i++) {
                int rc = i & 1 ? fbdev_fill_to(&d, &rectangles[i], 0x619BD7u + i) :
                                 fbdev_present_to(&d, &s, &rectangles[i]);
                expected_pixels(&d, expected, i);
                if (rc || memcmp(d.mapped, expected, size))
                    (*errors)++;
            }
            const struct fb_rect invalid[] = {
                { INT32_MAX, 0, 1, 1 }, { 0, INT32_MAX, 1, 1 },
                { 0, 0, -1, 1 }, { 0, 0, 1, -1 },
            };
            for (unsigned i = 0; i < ARRAY_SIZE(invalid); i++) {
                if (fbdev_present_to(&d, &s, &invalid[i]) != -EINVAL ||
                    fbdev_fill_to(&d, &invalid[i], 0) != -EINVAL)
                    (*errors)++;
            }
            struct fb_surface bad = s;
            bad.pitch = SW * 4 - 1;
            if (fbdev_present_to(&d, &bad, &rectangles[0]) != -EINVAL ||
                memcmp(d.mapped, expected, size))
                (*errors)++;
            for (unsigned i = 0; i < GUARD; i++)
                if (raw[i] != SENTINEL || raw[GUARD + size + i] != SENTINEL)
                    (*guards)++;
            for (unsigned y = 0; y < FH; y++)
                for (unsigned x = FW * (bpp / 8); x < pitch; x++)
                    if (d.mapped[y * pitch + x] != SENTINEL)
                        (*guards)++;
            uint32_t actual = fnv1a32(d.mapped, size, 2166136261u);
            uint32_t reference = fnv1a32(expected, size, 2166136261u);
            if (actual != reference)
                (*errors)++;
            *digest = fnv1a32(&actual, sizeof(actual), *digest);
            rec_emit("framebuffer", "DATA", "group=fixture bpp=%u pitch=%u digest=%08x reference=%08x guard_errors=%u errors=%u",
                     bpp, pitch, actual, reference, *guards, *errors);
            kfree(expected);
            kfree(raw);
        }
    }
    kfree(source);
    return 0;
}

static int live_pattern(const struct fb_device *d)
{
    static const uint32_t colours[] = { 0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00,
                                       0xFF00FF, 0xFF0000, 0x0000FF, 0x000000 };
    for (unsigned i = 0; i < ARRAY_SIZE(colours); i++) {
        int32_t x = (uint32_t)((uint64_t)d->width * i / ARRAY_SIZE(colours));
        int32_t end = (uint32_t)((uint64_t)d->width * (i + 1) / ARRAY_SIZE(colours));
        struct fb_rect r = { x, 0, end - x, (int32_t)d->height };
        int rc = fbdev_fill(&r, colours[i]);
        if (rc)
            return rc;
    }
    const struct fb_rect frame[] = {
        { 0, 0, (int32_t)d->width, 1 }, { 0, (int32_t)d->height - 1, (int32_t)d->width, 1 },
        { 0, 0, 1, (int32_t)d->height }, { (int32_t)d->width - 1, 0, 1, (int32_t)d->height },
    };
    for (unsigned i = 0; i < ARRAY_SIZE(frame); i++) {
        int rc = fbdev_fill(&frame[i], 0xFFFFFF);
        if (rc)
            return rc;
    }
    return 0;
}

int probe_framebuffer(void)
{
    rec_emit("framebuffer", "BEGIN", 0);
    unsigned guards = 0, errors = 0;
    uint32_t digest = 2166136261u;
    int rc = fixtures(&digest, &guards, &errors);
    if (!rc)
        rc = fbdev_init();
    const struct fb_device *d = fbdev_get();
    uint16_t mode = d->mode_id;
    bool absent = (g_boot.flags & CBI_F_TEXT_MODE) || !g_boot.fb_phys;
    if (!rc && absent) {
        struct fb_rect r = { 0, 0, 1, 1 };
        if (fbdev_present(0, &r) != -ENODEV || fbdev_fill(&r, 0) != -ENODEV)
            errors++;
    } else if (!rc) {
        rc = live_pattern(d);
    }
    if (mode != d->mode_id || (!absent && mode != g_boot.vbe_mode))
        errors++;
    rec_emit("framebuffer", "DATA", "group=mode mode=%04x width=%u height=%u bpp=%u pitch=%u absent=%u owner=fbdev generation=%u",
             d->mode_id, d->width, d->height, d->bpp, d->pitch, absent, d->generation);
    rec_emit("framebuffer", "DATA", "group=masks red=%u:%u green=%u:%u blue=%u:%u reserved=%u:%u",
             d->red_size, d->red_pos, d->green_size, d->green_pos, d->blue_size, d->blue_pos,
             d->rsvd_size, d->rsvd_pos);
    bool pass = !rc && !guards && !errors;
    rec_emit("framebuffer", "DATA", "digest=%08x guard_errors=%u errors=%u mode_calls=0 error=%d absent=%u",
             digest, guards, errors, rc, absent);
    rec_emit("framebuffer", "END", "status=%s reason=%s", pass ? "PASS" : "FAIL",
             pass ? "pixels_guards_mode" : "framebuffer_error");
    /* Evidence uses the existing screen sink, so refresh after its drawing.
     * The lead's later paging/console writes still need shared ownership. */
    if (pass && !absent)
        return live_pattern(d);
    return pass ? 0 : -EFAULT;
}

CIUKI_F1_PROBE("framebuffer", probe_framebuffer);
