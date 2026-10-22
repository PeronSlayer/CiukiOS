/* CPU-mapped visible targets only. Caller validates geometry and owns drawing,
 * proves engine idle and drains stores before releasing ownership.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_SWDIB_H
#define CIUKI_SWDIB_H
#include <ciuki/fbdev.h>
uint32_t swdib_pack(const struct fb_device *, uint32_t);
void swdib_present(const struct fb_device *, const struct fb_surface *, const struct fb_rect *, const struct fb_rect *);
void swdib_fill(const struct fb_device *, const struct fb_rect *, uint32_t native);
void swdib_copy(const struct fb_device *, const struct fb_rect *, int32_t, int32_t);
void swdib_pattern(const struct fb_device *, const struct fb_rect *, const uint8_t[8], uint32_t, uint32_t);
void swdib_convert(const struct fb_device *, const struct fb_rect *, void *, uint32_t);
void swdib_write(const struct fb_device *, const struct fb_rect *, const struct fb_rect *, const void *, uint32_t);
#endif
