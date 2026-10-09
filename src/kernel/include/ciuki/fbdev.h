/* Boot framebuffer device and integer damage presenter (F1).
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_FBDEV_H
#define CIUKI_FBDEV_H

#include <stdint.h>
#include <stdbool.h>
#include <ciuki/sync.h>

#ifndef ENODEV
#define ENODEV 19
#endif

struct fb_rect { int32_t x, y, width, height; };
struct fb_surface {
    const void *pixels;             /* little-endian 0xXXRRGGBB; X ignored */
    uint32_t width, height, pitch;   /* pitch in bytes, may be unaligned */
    uint32_t size;                  /* accessible allocation extent in bytes */
};
struct fb_device {
    uint32_t width, height, pitch, size, phys;
    uint8_t *mapped;
    uint8_t bpp;
    uint8_t red_size, red_pos, green_size, green_pos, blue_size, blue_pos;
    uint8_t rsvd_size, rsvd_pos;
    uint16_t mode_id;              /* immutable loader-selected mode */
    bool present;
    int handle;
    gen_t generation;
};

/* Call once after registry/vmm initialization, in thread context with IF=1.
 * Missing LFB succeeds with present=false. Invalid metadata/claim fails
 * before framebuffer access. Further calls retain the original mode. */
int fbdev_init(void);
const struct fb_device *fbdev_get(void);
bool fbdev_rect_valid(const struct fb_rect *rect);
/* No scaling: source (0,0) maps to dst (x,y). dst dimensions must fit src.
 * Clip at all screen edges, advancing the source for left/top clipping.
 * Empty/outside rectangles succeed; invalid requests write nothing.
 * Source storage must remain readable and disjoint from the destination.
 * Live calls require thread context with IF=1 and serialize presenters
 * with a sleeping mutex, keeping IF=1.
 * Console synchronization needs lead plumbing (console.c is out of scope). */
int fbdev_present(const struct fb_surface *src, const struct fb_rect *dst);
int fbdev_fill(const struct fb_rect *rect, uint32_t colour);
/* Same production renderer for private shadow fixtures. These do not claim
 * hardware or mutate the live device. Caller owns and serializes the target;
 * metadata must describe the complete mapped allocation. */
int fbdev_present_to(const struct fb_device *dev, const struct fb_surface *src,
                     const struct fb_rect *dst);
int fbdev_fill_to(const struct fb_device *dev, const struct fb_rect *rect, uint32_t colour);
int probe_framebuffer(void);

#endif
