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
    /* f7-27: optional 3D owner, closed before the 2D stop proof runs. */
    int (*render3d_stop)(struct m64_engine *);
    uint32_t vram_bytes;  /* MEM_CNTL size the driver already validated */
    uint32_t mem_cntl;    /* Read-only boot evidence, before size caps. */
    bool render3d_qualified;
    uint64_t render3d_writes;
};
void m64_attach(struct m64_engine *, const struct fb_device *, const struct m64_io *, uint8_t *, uint32_t);
int m64_configure(struct m64_engine *);
void m64_quarantine(struct m64_engine *, const char *);
int m64_idle_proof(struct m64_engine *);
bool m64_format(const struct fb_device *);

/* f7-27 3D transport: CPU MMIO register writes only (no bus-master DMA,
 * no AGP). Offsets use the 2D driver's BAR2-page convention (registers.h);
 * the setup-engine block 1 sits 0x400 below block 0, as X.Org derives
 * Block1Base = Block0Base - 0x400 (atipreinit.c:1939). One budget covers
 * every FIFO wait and the final idle proof of a 3D window. */
struct m64_write { uint32_t reg, value; };
struct m64_budget { uint64_t start; unsigned reads; };
#define M64_3D_WINDOW_US 20000u
#define M64_3D_READ_LIMIT 262144u
#define M64_3D_WRITE_LIMIT 8192u
bool m64_render3d_writable(unsigned);
bool m64_render3d_readable(unsigned);
int m64_render3d_write(struct m64_engine *, const struct m64_write *, unsigned, struct m64_budget *);
int m64_render3d_idle(struct m64_engine *, struct m64_budget *);
int m64_render3d_read(struct m64_engine *, unsigned, uint32_t *);
#endif
