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
    int (*delay_us)(void *, uint32_t);
    void (*drain)(void *);
    void (*fault)(void *);
    uint8_t (*read8)(void *, uint32_t);
    void (*write8)(void *, uint32_t, uint8_t);
    /* Byte VGA I/O only: inclusive 0x3b0, exclusive 0x3e0. */
    uint8_t (*read_port)(void *, uint16_t);
    void (*write_port)(void *, uint16_t, uint8_t);
};
struct savage2d {
    struct savage_io io;
    struct accel_ops ops;
    uint32_t width, height, descriptor, offset, cursor_offset;
    uint8_t *cursor;
    uint8_t saved40, saved31, saved50, saved45, saved55;
    uint32_t saved_bci, saved_gbd[2];
    uint32_t idle_mask, idle_value;
    const char *reason;
    uint64_t operations, idle_waits, fallbacks, timeouts;
    uint64_t reset_us, poll_us;
    bool active, quarantined, cpu_safe, touched, supersavage, cursor_loaded, status_logged;
    bool mmio_saved, status_ready;
};
bool savage2d_supported(const struct pci_func *);
/* Attempt marker file I/O for kernel callers, outside presenter locks. */
struct vfs;
int savage2d_attempt_begin(struct vfs *);
int savage2d_attempt_end(struct vfs *);
void savage2d_quarantine(const char *reason);
int savage2d_start(struct savage2d *, const struct savage_io *, const struct fb_device *,
                   bool supersavage, uint8_t *cursor, uint32_t cursor_offset);
#endif
