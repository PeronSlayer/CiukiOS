/* Private MMIO port boundary. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_RADEON2D_ENGINE_H
#define CIUKI_RADEON2D_ENGINE_H
#include <ciuki/display_driver.h>
#include "registers.h"
#define RADEON_WAIT_US 20000u
#define RADEON_POLL_US 5u
#define RADEON_POLLS (RADEON_WAIT_US / RADEON_POLL_US + 1u)
#define RADEON_CURSOR_BYTES 4096u
/* Private feature masks; the legacy ABI still exposes only bits 0 and 1.
 * Optional commands use their existing part A hook-presence contracts. */
#define RADEON_RECT 1u
#define RADEON_CURSOR 2u
#define RADEON_PATTERN 4u
#define RADEON_WRITE 8u
#define RADEON_UPLOAD_WORDS 48u /* setup + all payload fits 64 FIFO slots */
enum radeon_group { RADEON_UNKNOWN, RADEON_R100, RADEON_R200, RADEON_R300, RADEON_R4XX };
struct radeon_group_info {
    const char *name;
    unsigned features;
    uint32_t cache_bits;
};
struct radeon_io {
    void *ctx;
    uint32_t (*read)(void *, uint32_t);
    void (*write)(void *, uint32_t, uint32_t);
    uint64_t (*now)(void *);
    int (*delay)(void *, uint32_t);
    void (*drain)(void *);
};
struct radeon_engine {
    struct radeon_io io;
    struct accel_ops ops;
    struct fb_device fb;
    uint32_t datatype, local_base, pitch_offset, status;
    uint32_t memory_location;
    uint64_t local_bytes;
    const struct radeon_group_info *group;
    unsigned verified;
    uint32_t cache_mode;
    bool testing;
    uint8_t *cursor;
    uint32_t cursor_offset;
    bool active, quarantined, cpu_safe, touched, cursor_loaded, cursor_hidden, unowned;
    const char *reason;
    uint64_t commands, waits, timeouts;
    int (*pause)(void *);
    void *pause_arg;
};
bool radeon_format(const struct fb_device *, uint32_t *);
int radeon_engine_prepare(struct radeon_engine *, const struct fb_device *, const struct radeon_io *, enum radeon_group);
/* Entire setup and all pixel access run inside fbdev_accel_probe. */
int radeon_engine_activate(void *);
void radeon_engine_quarantine(struct radeon_engine *, const char *);
int radeon_engine_idle(struct radeon_engine *);
int radeon_cursor_hide(struct radeon_engine *);
int radeon_cursor_set(void *, const struct accel_cursor *);
int radeon_cursor_move(void *, int32_t, int32_t);
int radeon_cursor_show(void *, bool);
extern const struct display_driver radeon2d_driver;
#endif
