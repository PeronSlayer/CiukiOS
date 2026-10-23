/*
 * Copyright (c) 1993-1999 NVIDIA, Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
/* Register constants derived from src/riva_hw.h and riva_setup.c, X.Org
 * xf86-video-nv 0b6619fec39fada995646fb95bb25e72d3c9a491. */
/* CiukiOS private NV3 host boundary. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NV3_PRIVATE_H
#define CIUKI_NV3_PRIVATE_H
#include <ciuki/display_driver.h>

/* Logical MMIO offsets; INSTANCE is separately mapped from BAR1, not BAR0. */
#define NV3_INSTANCE 0xc00000u
#define NV3_INSTANCE_BYTES 0x8000u
#define NV3_CURSOR (NV3_INSTANCE + 0x7800u)
#define NV3_PFIFO 0x2000u
#define NV3_PGRAPH 0x400000u
#define NV3_PFB 0x100000u
#define NV3_FIFO 0x800000u
#define NV3_ROP NV3_FIFO
#define NV3_CLIP (NV3_FIFO + 0x2000u)
#define NV3_PATTERN (NV3_FIFO + 0x4000u)
#define NV3_BLT (NV3_FIFO + 0x8000u)
#define NV3_BITMAP (NV3_FIFO + 0xa000u)
#define NV3_CRTC_INDEX 0x6013d4u
#define NV3_CRTC_DATA 0x6013d5u
#define NV3_SEQ_INDEX 0x0c03c4u
#define NV3_SEQ_DATA 0x0c03c5u
#define NV3_CURSOR_POSITION 0x680300u
#define NV3_WAIT_US 20000u
#define NV3_POLL_CAP 4001u

struct nv3_io {
    void *ctx;
    uint32_t (*read32)(void *, uint32_t);
    void (*write32)(void *, uint32_t, uint32_t);
    uint16_t (*read16)(void *, uint32_t);
    uint8_t (*read8)(void *, uint32_t);
    void (*write8)(void *, uint32_t, uint8_t);
    uint64_t (*now_us)(void *);
    int (*delay_us)(void *, uint32_t);
    void (*drain)(void *);
};
struct nv3_engine {
    struct nv3_io io;
    struct fb_device fb;
    uint32_t ram_bytes;
    uint16_t empty_count;
    bool active, quarantined, cpu_safe, touched, configured;
    bool cursor_hidden, cursor_loaded;
    const char *reason;
    uint64_t operations, waits, fallbacks;
    int (*pause)(void *);
    void *pause_ctx;
    struct accel_ops ops;
};
/* init is called only under the presenter's exclusive probe. */
int nv3_engine_init(struct nv3_engine *, const struct nv3_io *, const struct fb_device *, uint32_t);
void nv3_engine_quarantine(struct nv3_engine *, const char *);
int nv3_engine_cursor_test(struct nv3_engine *, uint64_t, bool (*)(void *), void *);
bool nv3_mode_supported(const struct fb_device *);
uint32_t nv3_detect_vram(const struct nv3_io *);
extern const struct display_driver riva128_driver;
#endif
