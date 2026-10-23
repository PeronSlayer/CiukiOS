/* Private synchronous native-port helpers. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_VIDEO_NATIVE_H
#define CIUKI_VIDEO_NATIVE_H
#include <ciuki/kernel.h>
#include <ciuki/display_driver.h>

/* All device I/O, including indexed VGA selectors, is injectable. */
struct video_io {
    void *ctx;
    uint32_t (*read)(void *, unsigned, uint32_t);
    void (*write)(void *, unsigned, uint32_t, uint32_t);
    uint8_t (*indexed_read)(void *, uint16_t, uint8_t);
    void (*indexed_write)(void *, uint16_t, uint8_t, uint8_t);
    uint64_t (*now)(void *);
    void (*drain)(void *);
};
struct video_access {
    int handle;
    gen_t generation;
    void *window, *mapped;
    uint32_t bytes;
};
struct video_native {
    const char *id, *name, *marker, *reason;
    struct video_io io;
    struct fb_device fb;
    struct accel_ops ops;
    bool ready, quarantined, cpu_safe, touched, shape, cursor_available;
    bool attempted, armed, pending_record;
    uint64_t operations, waits;
    int (*idle)(struct video_native *); /* 0 done, -EAGAIN busy, other fault */
    int (*hide)(struct video_native *);
    int (*restore)(struct video_native *); /* only after idle + hidden proof */
    int (*pause)(void *);
    void *pause_arg;
    uint8_t palette_saved[6], dac_mask, dac_index;
    struct video_access access[6];
    unsigned access_count;
};
#define VIDEO_WAIT_US 20000u
#define VIDEO_WAIT_POLLS 4096u
uint32_t video_read(struct video_native *, unsigned, uint32_t);
void video_write(struct video_native *, unsigned, uint32_t, uint32_t);
int video_wait(struct video_native *, int (*)(struct video_native *));
int video_fault(struct video_native *, const char *, int);
int video_sync(void *);
int video_stop(void *);
bool video_cpu_ready(void *);
unsigned video_capabilities(void *);
void video_pause(void *, int (*)(void *), void *);
bool video_rect(const struct video_native *, const struct fb_rect *);
bool video_format(const struct fb_device *);
void video_cursor_bits(uint8_t[1024], const struct accel_cursor *);
uint32_t video_cursor_rgb(const struct fb_device *, uint32_t);
/* Caller temporarily selects the chip's extended palette bank. */
void video_palette_save(struct video_native *);
void video_palette_load(struct video_native *, const uint8_t[6]);
bool video_reserved(uint32_t, uint32_t);
bool video_disjoint(uint32_t, uint32_t, uint32_t, uint32_t);
void *video_map(struct video_native *, enum res_type, uint32_t, uint32_t);
bool video_claim(struct video_native *, uint32_t, uint32_t);
void video_release(struct video_native *);
void video_hardware_io(struct video_native *, void *);
int video_marker_begin(struct video_native *, struct vfs *);
int video_marker_end(struct video_native *, struct vfs *);
int video_marker_failed(struct video_native *, struct vfs *);
void video_record(struct video_native *);
void video_snapshot(struct video_native *, struct ciuki_accel_info *);
bool video_summary(struct video_native *, bool *, bool *, const char **);
int video_probe_pixels(void *);
int video_cursor_test(const struct accel_ops *, uint64_t, bool (*)(void *), void *);
#endif
