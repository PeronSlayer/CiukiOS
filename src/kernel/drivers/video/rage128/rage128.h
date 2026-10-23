/* Private CiukiOS host boundary. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_PRIVATE_RAGE128_H
#define CIUKI_PRIVATE_RAGE128_H
#include <ciuki/display_driver.h>
#define R128_MMIO_BYTES 0x4000u
#define R128_WAIT_US 20000u
#define R128_POLL_LIMIT 65536u
struct r128_io {
    void *ctx;
    uint32_t (*read)(void *, unsigned);
    void (*write)(void *, unsigned, uint32_t);
    uint64_t (*now)(void *);
    void (*drain)(void *);
    bool (*valid)(void *);
};
struct r128_engine {
    struct r128_io io;
    struct fb_device fb;
    struct accel_ops ops;
    volatile uint8_t *cursor;
    uint32_t cursor_offset, master, rgb_mask, status;
    bool active, quarantined, cpu_safe, touched, submitted, cursor_loaded;
    bool cursor_hidden;
    const char *reason;
    uint64_t operations, idle_waits, fallbacks;
    int (*pause)(void *);
    void *pause_arg;
};
bool r128_format(const struct fb_device *);
int r128_engine_init(struct r128_engine *, const struct fb_device *, struct r128_io,
                     volatile uint8_t *, uint32_t);
void r128_quarantine(struct r128_engine *, const char *);
/* Staged for f2-101d: no public opcode or capability allocated here. */
int r128_invert(struct r128_engine *, const struct fb_rect *);
extern const struct display_driver rage128_driver;
#endif
