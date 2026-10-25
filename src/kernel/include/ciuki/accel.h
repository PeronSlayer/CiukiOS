/* Synchronous optional framebuffer acceleration; framebuffer mutex owns calls.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_ACCEL_H
#define CIUKI_ACCEL_H
#include <ciuki/fbdev.h>
struct accel_cursor {
    uint32_t mask[32], image[32]; /* 32x32, left pixel in bit 31; image selects fg */
    uint32_t foreground, background; /* native framebuffer pixel values */
};
struct swdib_command;
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
    /* Part C: whole-record preparation/submission. Receives only immutable
     * kernel staging and retained targets, with one clip union. A refusal
     * must precede ALL fragments and cursor changes. Success proves idle.
     * Only scanout/video targets are eligible; system surfaces use swdib.
     * command_bits uses (1u << CIUKI_CMD_*). ARGB is exact premultiplied
     * composition at current bpp, otherwise it must not be advertised. */
    uint32_t (*command_bits)(void *);
    int (*command)(void *, const struct swdib_command *);
    void (*cursor_limits)(void *, uint32_t *width, uint32_t *height);
    uint32_t (*command_features)(void *); /* optional BILINEAR / STRETCH bits */
    /* Optional exact edge-sampled SCALED (CIUKI_CAP_STRETCH). A clean refusal
     * precedes every fragment. Same retained targets/clip union as command. */
    int (*stretch_rect)(void *, const struct swdib_command *);
};
struct vfs;
/* -EOPNOTSUPP guarantees no submission and no modified pixels. Success
 * proves completion. Any other error may have partially executed. */
#endif
