/* Synchronous optional framebuffer acceleration; framebuffer mutex owns calls.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_ACCEL_H
#define CIUKI_ACCEL_H
#include <ciuki/fbdev.h>
struct accel_cursor {
    uint32_t mask[32], image[32]; /* 32x32, left pixel in bit 31; image selects fg */
    uint32_t foreground, background; /* native framebuffer pixel values */
};
struct accel_ops {
    void *ctx;
    int (*sync)(void *);
    int (*copy_rect)(void *, const struct fb_rect *, int32_t, int32_t);
    int (*fill_rect)(void *, const struct fb_rect *, uint32_t);
    int (*pattern_rect)(void *, const struct fb_rect *, const uint8_t[8], uint32_t, uint32_t);
    int (*cursor_set)(void *, const struct accel_cursor *);
    int (*cursor_move)(void *, int32_t, int32_t);
    int (*cursor_show)(void *, bool);
    int (*stop)(void *);
    unsigned (*capabilities)(void *); /* bit 0 rectangles, bit 1 cursor */
    bool (*cpu_ready)(void *); /* cached, nonwaiting console/panic check */
    /* f2-92, optional (NULL: not supported). Native-format rows, `stride`
     * bytes apart, for destination `dst`; only pixels inside `clip` are
     * written. Rows may be visible or engine-only (offscreen). */
    int (*write_rect)(void *, const struct fb_rect *dst, const struct fb_rect *clip,
                      const void *rows, uint32_t stride);
    /* Engine-only rows [*first, result); 0 when there are none. */
    uint32_t (*offscreen)(void *, uint32_t *first);
    /* The display service's completion waits run in 1 ms slices; between
     * slices `pause` releases the presenter lock (NULL: unsliced, bounded). */
    void (*set_pause)(void *, int (*pause)(void *), void *arg);
    /* Once, before the first write or offscreen use: prove both on the
     * hardware (saved block, LFB readback). Caller holds the presenter lock
     * and a full-screen drawing claim. 0: writes usable. */
    int (*verify_write)(void *);
};
struct vfs;
const struct accel_ops *savage2d_init(const struct fb_device *, struct vfs *);
#endif
