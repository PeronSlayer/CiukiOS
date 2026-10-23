/* Private NeoMagic port interfaces. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NEO_H
#define CIUKI_NEO_H
#include <ciuki/display_driver.h>
#include "regs.h"
#define NEO_POLL_LIMIT 65536u
#define NEO_TIMEOUT_US 20000u
#define NEO_CHECK_US 100000u
struct neo_io {
    void *ctx;
    uint32_t (*read)(void *, unsigned);
    void (*write)(void *, unsigned, uint32_t);
    uint64_t (*now)(void *);
    void (*drain)(void *);
    bool (*hide)(void *); /* independent GR82 disable, including lost MMIO */
};
struct neo_engine {
    struct neo_io io;
    struct fb_device fb;
    struct accel_ops ops;
    bool active, quarantined, cpu_safe, cursor_safe, loaded, shown, hidden;
    bool touched, submitted, configured;
    uint8_t *cursor;
    uint32_t cursor_offset;
    struct accel_cursor shape;
    int32_t cursor_x, cursor_y;
    uint64_t operations, waits, timeouts;
    uint64_t check_since;
    bool checking;
    const char *reason;
    int (*pause)(void *);
    void *pause_ctx;
};
bool neo_chip(uint16_t);
uint32_t neo_vram(uint16_t);
bool neo_geometry(const struct fb_device *);
int neo_setup(struct neo_engine *, const struct neo_io *, const struct fb_device *, uint8_t *, uint32_t);
int neo_dm9(void *);
int neo_idle(struct neo_engine *);
int neo_stop(void *);
void neo_quarantine(struct neo_engine *, const char *);
int neo_hide(struct neo_engine *);
int neo_cursor_set(void *, const struct accel_cursor *);
int neo_cursor_move(void *, int32_t, int32_t);
int neo_cursor_show(void *, bool);
/* Optional donor primitives, private until Part C allocates hooks/bits.
 * No public operation number, capability or accel_ops field is invented. */
int neo_mono(void *, const struct fb_rect *, const void *, uint32_t, uint32_t, uint32_t, bool);
int neo_invert(void *, const struct fb_rect *);
extern const struct display_driver neomagic_driver;
#endif
