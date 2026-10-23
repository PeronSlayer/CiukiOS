/* Private GD54xx synchronous engine boundary. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_CIRRUS_ENGINE_H
#define CIUKI_CIRRUS_ENGINE_H
#include <ciuki/display_driver.h>
#define CIRRUS_WAIT_US 20000u
#define CIRRUS_WAIT_POLLS 65536u
struct cirrus_io {
    void *ctx;
    uint8_t (*read)(void *, uint8_t);
    void (*write)(void *, uint8_t, uint8_t);
    uint64_t (*now)(void *);
    void (*drain)(void *);
    bool (*valid)(void *);
};
struct cirrus_engine {
    struct accel_ops ops;
    struct cirrus_io io;
    struct fb_device fb;
    bool active, quarantined, cpu_safe, touched;
    uint8_t status;
    const char *reason;
    uint64_t operations, waits, timeouts;
    unsigned max_width, max_height;
    int (*pause)(void *);
    void *pause_arg;
};
bool cirrus_layout(const struct fb_device *);
int cirrus_engine_start(struct cirrus_engine *, const struct fb_device *, const struct cirrus_io *, uint16_t);
void cirrus_engine_quarantine(struct cirrus_engine *, const char *);
#endif
