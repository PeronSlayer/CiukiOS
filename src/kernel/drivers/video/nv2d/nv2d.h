/* Private native adapter interface. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NV2D_PRIVATE_H
#define CIUKI_NV2D_PRIVATE_H
#include <ciuki/display_driver.h>
#define NV2D_MMIO_BYTES 0x810000u
#define NV2D_TAIL_BYTES 0x20000u
#define NV2D_NV40_TAIL_BYTES 0x100000u
#define NV2D_SCRATCH_BYTES 0x4000u
#define NV2D_RING_WORDS 8192u
#define NV2D_SKIPS 8u
#define NV2D_PUT (NV2D_FIFO + 0x40u)
#define NV2D_GET (NV2D_FIFO + 0x44u)
#define NV2D_FIFO 0x800000u
#define NV2D_FREE (NV2D_FIFO + 0x10u)
#define NV2D_GRAPH_BUSY 0x400700u
#define NV2D_GRAPH_INTR 0x400100u
#define NV2D_FIFO_INTR 0x2100u
#define NV2D_PCIO 0x601000u
#define NV2D_WAIT_US 20000u
#define NV2D_WAIT_POLLS 4000u
struct nv2d_io {
    void *ctx;
    uint32_t (*read)(void *, uint32_t);
    void (*write)(void *, uint32_t, uint32_t);
    uint8_t (*read8)(void *, uint32_t);
    void (*write8)(void *, uint32_t, uint8_t);
    uint64_t (*now)(void *);
    void (*delay)(void *, unsigned);
    void (*drain)(void *);
    bool (*valid)(void *);
};
struct nv2d {
    struct nv2d_io io;
    struct fb_device fb;
    struct accel_ops ops;
    uint16_t device;
    unsigned architecture, empty_bytes;
    uint32_t vram_bytes, scratch_offset, cursor_offset, head_offset, tail_bytes;
    uint32_t ring_offset, current, put;
    volatile uint32_t *ring;
    bool dma, ring_ready, wrap_pending;
    volatile uint8_t *tail;
    bool touched, active, quarantined, cpu_safe, cursor_hidden, cursor_set;
    bool alpha_cursor, cursor_saved;
    uint8_t saved_cursor[3], saved_lock;
    uint32_t saved_cursor_config, saved_general;
    uint64_t operations, waits, slice;
    int (*pause)(void *);
    void *pause_ctx;
    const char *reason;
};
unsigned nv2d_architecture(uint16_t);
uint16_t nv2d_chipset(const struct nv2d_io *, uint16_t);
uint32_t nv2d_tail_bytes(unsigned architecture);
bool nv2d_format(const struct fb_device *);
uint32_t nv2d_vram(const struct nv2d_io *, unsigned architecture);
int nv2d_engine_init(struct nv2d *, const struct nv2d_io *, const struct fb_device *,
                     uint16_t device, uint32_t vram_bytes, volatile uint8_t *tail);
int nv2d_setup(struct nv2d *);
int nv2d_dma_setup(struct nv2d *);
int nv2d_compare(struct nv2d *);
int nv2d_idle(struct nv2d *);
int nv2d_emit(struct nv2d *, uint32_t method, unsigned count, const uint32_t *);
void nv2d_quarantine(struct nv2d *, const char *);
int nv2d_cursor_hide(struct nv2d *);
extern const struct display_driver nv2d_driver;
#endif
