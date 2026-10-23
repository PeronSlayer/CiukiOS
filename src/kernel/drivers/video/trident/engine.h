/* Private Ciuki Blade/XP engine interface. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_TRIDENT_ENGINE_H
#define CIUKI_TRIDENT_ENGINE_H
#include <ciuki/display_driver.h>

#define TB_STATUS 0x2120u
#define TB_POLL_LIMIT 200000u
#define TB_WAIT_US 20000u
enum tb_kind { TB_NONE, TB_BLADE, TB_XP };
struct tb_io {
    void *ctx;
    uint8_t (*read8)(void *, unsigned);
    uint32_t (*read32)(void *, unsigned);
    void (*write8)(void *, unsigned, uint8_t);
    void (*write32)(void *, unsigned, uint32_t);
    uint64_t (*now_us)(void *);
    bool (*valid)(void *); /* optional, cached/nonwaiting access-lease check */
};
struct tb_engine {
    struct tb_io io;
    struct accel_ops ops;
    const struct fb_device *fb;
    enum tb_kind kind;
    bool active, quarantined, cpu_safe;
    uint8_t operation;
    uint32_t status;
    uint64_t operations, waits;
    const char *reason;
    int (*pause)(void *);
    void *pause_ctx;
};
enum tb_kind tb_device(uint16_t);
bool tb_format(enum tb_kind, const struct fb_device *);
void tb_construct(struct tb_engine *, enum tb_kind, const struct fb_device *, const struct tb_io *);
int tb_initialize(struct tb_engine *);
int tb_idle(struct tb_engine *);
void tb_quarantine(struct tb_engine *, const char *);
extern const struct display_driver trident_blade_driver;
#endif
