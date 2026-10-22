/* Boot framebuffer device and integer damage presenter (F1).
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_FBDEV_H
#define CIUKI_FBDEV_H

#include <stdint.h>
#include <stdbool.h>
#include <ciuki/sync.h>

#ifndef EBUSY
#define EBUSY 16
#endif

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
    uint16_t mode_id;              /* active VBE mode */
    bool present;
    int handle;
    gen_t generation;
};

/* Call once after registry/vmm initialization, in thread context with IF=1.
 * Missing LFB succeeds with present=false. Invalid metadata/claim fails
 * before framebuffer access. Further calls leave the active mode untouched. */
int fbdev_init(void);
struct vfs;
/* Queue only after storage mount and driver activation; never touches the
 * engine synchronously. Safe mode retains software presentation. */
void fbdev_accel_schedule(struct vfs *);
/* Worker-only saved-pixel probe. Register setup runs without this exclusion;
 * the pending initialization pins the mode until publication completes. */
int fbdev_accel_probe(const struct fb_device *, int (*probe)(void *), void *);
const struct fb_device *fbdev_get(void);
/* f1-23 transaction: submissions stop and both presenter/console mappings
 * are revoked before firmware. commit remaps validated, claimed geometry;
 * failure leaves the device unavailable until a confirmed restore commits. */
int fbdev_change_begin(void);
int fbdev_change_commit(const struct fb_device *next);
void fbdev_change_end(void);
int fbdev_mode_decode(uint16_t id, const uint8_t info[256], struct fb_device *out);
void console_framebuffer_changed(const struct fb_device *dev);
void console_framebuffer_revoke(void);
bool fbdev_rect_valid(const struct fb_rect *rect);
/* No scaling: source (0,0) maps to dst (x,y). dst dimensions must fit src.
 * Clip at all screen edges, advancing the source for left/top clipping.
 * Empty/outside rectangles succeed; invalid requests write nothing.
 * Source storage must remain readable and disjoint from the destination.
 * Live calls require thread context with IF=1 and serialize presenters
 * with a sleeping mutex, keeping IF=1.
 * Console owns pixel rows [0, rows), registered once during console setup
 * before live presentation by fbdev_console_region.
 * On this uniprocessor, console_begin/end bracket a short irq_save section
 * per text line; they never sleep or spin. Present/fill atomically claim
 * overlapping rows while IF=1 and return -EBUSY if the console is drawing.
 * Console skips pixels (retaining history) while a presenter owns its rows.
 * Private shadow renderers are independent. Panic stays lock-free: no mutex,
 * allocation or waiting is introduced; a preempted present may suppress pixels. */
void fbdev_console_region(unsigned rows);
bool fbdev_console_begin(void);
void fbdev_console_end(void);
/* Live damage operations: */
int fbdev_present(const struct fb_surface *src, const struct fb_rect *dst);
/* Internal bounded native row upload, <=256 pixels. Generation checked
 * under the presenter lock, acceleration drained, WC writes drained. */
int fbdev_present_native(uint32_t generation, unsigned x, unsigned y,
                          const void *pixels, unsigned count);
int fbdev_fill(const struct fb_rect *rect, uint32_t colour);
struct ciuki_accel_request;
int fbdev_accel_request(const struct ciuki_accel_request *);
/* f2-92 CIUKI_ACCEL_WRITE band (see fbdev.c): 0 engine, 1 CPU, <0 error. */
int fbdev_accel_write(uint32_t generation, const struct fb_rect *dst, const struct fb_rect *limit,
                      void *rows, uint32_t stride);
struct ciuki_accel_info;
int fbdev_accel_info(uint32_t generation, struct ciuki_accel_info *);
void fbdev_cursor_hide(void);
/* Strictly bounded screen copy, hardware only. No software LFB reads.
 * -EAGAIN requires reconstruction/presentation from the caller's RAM frame;
 * failed overlapping hardware copies may already have modified scanout. */
int fbdev_copy_to(const struct fb_device *, const struct fb_rect *, int32_t, int32_t);
/* Same production renderer for private shadow fixtures. These do not claim
 * hardware or mutate the live device. Caller owns and serializes the target;
 * metadata must describe the complete mapped allocation. */
int fbdev_present_to(const struct fb_device *dev, const struct fb_surface *src,
                     const struct fb_rect *dst);
int fbdev_fill_to(const struct fb_device *dev, const struct fb_rect *rect, uint32_t colour);
int probe_framebuffer(void);

#endif
