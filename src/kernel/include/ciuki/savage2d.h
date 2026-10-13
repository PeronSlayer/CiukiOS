/* Register transport also used by the host engine model.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_SAVAGE2D_H
#define CIUKI_SAVAGE2D_H
#include <ciuki/accel.h>
struct pci_func;
#define SAVAGE_MMIO_BYTES 0x80000u
#define SAVAGE_BCI 0x10000u
#define SAVAGE_STATUS 0x48c60u
struct savage_io {
    void *ctx;
    uint32_t (*read32)(void *, uint32_t);
    void (*write32)(void *, uint32_t, uint32_t);
    uint8_t (*read_cr)(void *, uint8_t);
    void (*write_cr)(void *, uint8_t, uint8_t);
    uint64_t (*now_us)(void *);
    void (*relax)(void *);
    void (*drain)(void *);
    void (*fault)(void *);
};
struct savage2d {
    struct savage_io io;
    struct accel_ops ops;
    uint32_t width, height, descriptor, offset, cursor_offset;
    uint8_t *cursor;
    uint8_t saved40, saved31, saved50, saved45, saved55;
    uint32_t saved_bci, saved_gbd[2];
    uint64_t operations, idle_waits, fallbacks, timeouts;
    bool active, quarantined, cpu_safe, touched, supersavage, cursor_loaded;
};
bool savage2d_supported(const struct pci_func *);
int savage2d_start(struct savage2d *, const struct savage_io *, const struct fb_device *,
                   bool supersavage, uint8_t *cursor, uint32_t cursor_offset);
#endif
