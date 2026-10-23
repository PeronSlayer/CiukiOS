/* Native SVGA protocol transport; no donor implementation code.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_VMWARESVGA_ENGINE_H
#define CIUKI_VMWARESVGA_ENGINE_H
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/display_driver.h>
/* The unmodified MIT header expects VMware's fixed-width type spellings. */
typedef uint32_t uint32;
typedef int32_t int32;
#include "svga_reg.h"
/* Deprecated definitions absent from the selected header. Protocol reference:
 * QEMU v9.2.0 hw/display/vmware_vga.c (no implementation imported). */
#define VMSVGA_CAP_FILL 1u
#define VMSVGA_CMD_FILL 2u
#define VMSVGA_FIFO_LIMIT (4u*1024*1024)
struct vmsvga_io {
    void *ctx;
    uint32_t (*read)(void *, unsigned);
    void (*write)(void *, unsigned, uint32_t);
    uint32_t (*fifo_read)(void *, unsigned); /* byte offsets */
    void (*fifo_write)(void *, unsigned, uint32_t);
    void (*drain)(void *);
    uint64_t (*now)(void *); /* monotonic microseconds */
    int (*delay)(void *, unsigned);
    bool (*valid)(void *);
};
struct vmsvga_engine {
    struct accel_ops ops;
    struct vmsvga_io io;
    struct fb_device fb;
    uint32_t caps, fifo_caps, fifo_size, min, next;
    uint32_t packets, fills, copies, timeouts, status;
    uint32_t packet[8+32+32*32];
    bool initialized, active, quarantined, cpu_safe, pending, cursor, bypass3;
    bool cursor_loaded, cursor_visible;
    int32_t cursor_x, cursor_y;
    const char *reason;
    int (*pause)(void *);
    void *pause_ctx;
};
/* Caller owns validated BAR0/BAR2 resources and the active framebuffer. */
int vmsvga_engine_init(struct vmsvga_engine *, const struct vmsvga_io *,
                       const struct fb_device *, uint32_t fifo_phys, uint32_t fifo_bytes);
void vmsvga_engine_quarantine(struct vmsvga_engine *, const char *);
int vmsvga_engine_cursor_test(struct vmsvga_engine *, uint64_t,
                             bool (*)(void *), void *);
/* Production port/mapping boundary; replaced by fake registers on the host. */
struct vmsvga_ports { uint16_t base; volatile uint32_t *fifo; };
struct vmsvga_io vmsvga_port_io(struct vmsvga_ports *);
extern const struct display_driver vmwaresvga_driver;
#endif
