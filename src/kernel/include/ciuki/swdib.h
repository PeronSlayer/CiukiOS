/* CPU-mapped visible targets only. Caller validates geometry and owns drawing,
 * proves engine idle and drains stores before releasing ownership.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_SWDIB_H
#define CIUKI_SWDIB_H
#include <ciuki/fbdev.h>
#include <ciuki/abi.h>
/* Retained, immutable resolved view. Identity is the backing object, offset
 * is its byte origin; neither user handles nor virtual addresses prove aliasing.
 * Rights use PROT_READ/PROT_WRITE. Engine-only mappings have mapped=NULL. */
struct swdib_target {
    struct fb_device device;
    const void *identity;
    uint64_t offset;
    uint32_t rights, kind;
    uint32_t cache_epoch;
};
struct swdib_command {
    const struct ciuki_batch_record *record;
    const struct swdib_target *dst, *src;
    const struct ciuki_batch_clip *clips;
    const uint8_t *data;
};
int swdib_target_valid(const struct swdib_target *);
int swdib_validate(const struct swdib_command *);
bool swdib_footprint(const struct swdib_command *);
/* Validated/staged command only. pause is at completed CPU tile boundaries;
 * cancellation is observed by the executor after this record completes. */
void swdib_execute(const struct swdib_command *, void (*pause)(void *), void *);
uint32_t swdib_cursor_argb(const struct fb_device *, uint32_t, uint32_t);
uint32_t swdib_cursor_mono(const struct fb_device *, uint32_t, unsigned, unsigned, uint32_t, uint32_t);
uint32_t swdib_pack(const struct fb_device *, uint32_t);
void swdib_present(const struct fb_device *, const struct fb_surface *, const struct fb_rect *, const struct fb_rect *);
void swdib_fill(const struct fb_device *, const struct fb_rect *, uint32_t native);
void swdib_copy(const struct fb_device *, const struct fb_rect *, int32_t, int32_t);
void swdib_pattern(const struct fb_device *, const struct fb_rect *, const uint8_t[8], uint32_t, uint32_t);
void swdib_convert(const struct fb_device *, const struct fb_rect *, void *, uint32_t);
void swdib_write(const struct fb_device *, const struct fb_rect *, const struct fb_rect *, const void *, uint32_t);
#endif
