/* Private CiukiOS adapter interfaces. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_TDFX_ENGINE_H
#define CIUKI_TDFX_ENGINE_H
#include <ciuki/display_driver.h>

#define TDFX_MMIO_BYTES 0x201000u
#define TDFX_CURSOR_BYTES 1024u
#define TDFX_WAIT_US 20000u
#define TDFX_POLL_LIMIT 4001u
struct tdfx_io {
    void *ctx;
    uint32_t (*read)(void *, uint32_t);
    void (*write)(void *, uint32_t, uint32_t);
    uint64_t (*now)(void *);
    int (*delay)(void *, uint32_t);
    void (*drain)(void *);
};
struct tdfx_engine {
    struct accel_ops ops;
    struct tdfx_io io;
    struct fb_device fb;
    uint8_t *cursor;
    uint32_t cursor_offset, format, status;
    bool active, quarantined, cpu_safe, touched, shape, hidden, positioned;
    bool wanted;
    int32_t cursor_x, cursor_y;
    const char *reason;
    uint64_t operations, idle_waits, fallbacks;
    int (*pause)(void *);
    void *pause_arg;
};
bool tdfx_layout(const struct fb_device *);
uint32_t tdfx_vram(const struct tdfx_io *, uint16_t);
int tdfx_engine_start(struct tdfx_engine *, const struct tdfx_io *,
                      const struct fb_device *, uint8_t *, uint32_t);
void tdfx_engine_quarantine(struct tdfx_engine *, const char *);
int tdfx_cursor_check(const struct accel_ops *, uint64_t, bool (*)(void *), void *);
extern const struct display_driver tdfx_driver;
#endif
