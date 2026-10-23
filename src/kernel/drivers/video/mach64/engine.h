/* CiukiOS private Mach64 interface. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_MACH64_ENGINE_H
#define CIUKI_MACH64_ENGINE_H
#include <ciuki/accel.h>
#include <ciuki/abi.h>
struct m64_io {
    void *ctx;
    uint32_t (*read)(void *, unsigned);
    void (*write)(void *, unsigned, uint32_t);
    uint64_t (*now)(void *); /* monotonic microseconds */
    void (*drain)(void *);
};
struct m64_engine {
    struct m64_io io;
    struct fb_device fb;
    struct accel_ops ops;
    uint8_t *cursor;
    uint32_t cursor_offset;
    uint64_t commands;
    unsigned timeouts;
    bool active, quarantined, cpu_safe, touched;
    bool cursor_loaded, cursor_visible, cursor_outside, cursor_hidden;
    const char *reason;
    int (*pause)(void *);
    void *pause_arg;
};
void m64_attach(struct m64_engine *, const struct fb_device *, const struct m64_io *, uint8_t *, uint32_t);
int m64_configure(struct m64_engine *);
void m64_quarantine(struct m64_engine *, const char *);
int m64_idle_proof(struct m64_engine *);
bool m64_format(const struct fb_device *);
#endif
