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
};
struct vfs;
const struct accel_ops *savage2d_init(const struct fb_device *, struct vfs *);
#endif
