/* Ciuki private transport interface. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_INTEL2D_PRIVATE_H
#define CIUKI_INTEL2D_PRIVATE_H
#include <ciuki/display_driver.h>

#define INTEL_RING_BYTES 65536u
#define INTEL_DMA_PAGES 25u /* ring (16), physical HWS (1), scanline scratch (8) */
#define INTEL_DMA_BYTES (INTEL_DMA_PAGES * 4096u)
#define INTEL_TAIL 0x2030u
#define INTEL_HEAD 0x2034u
#define INTEL_START 0x2038u
#define INTEL_CTL 0x203cu
#define INTEL_HWS 0x2080u
#define INTEL_MODE 0x209cu
#define INTEL_INSTDONE 0x2090u
#define INTEL_HIC 0x70u
#define INTEL_PGTBL 0x2020u
#define INTEL_IDLE (1u << 9)
#define INTEL_SEQ 0x40u
#define INTEL_SCRATCH 0x80u
#define INTEL_WAIT_US 50000u
#define INTEL_POLL_LIMIT 100000u

/* Per-chip evidence gate; see intel-ring-gtt-reservation.md, Amendments 1 and 2. */
struct intel_chip {
    uint16_t device, bridge;
    bool gen2;
    uint32_t idle_mask;
    const char *refusal;
};
const struct intel_chip *intel_chip_lookup(uint16_t);

/* One deadline, presenter slice and frozen-clock cap for the entire call. */
struct intel_wait {
    uint64_t deadline, slice;
    unsigned polls;
};
struct intel_io {
    void *ctx;
    uint32_t (*read)(void *, uint32_t);
    void (*write)(void *, uint32_t, uint32_t);
    uint32_t (*pte)(void *, unsigned);
    void (*set_pte)(void *, unsigned, uint32_t);
    /* Clean WB storage, order CPU WC writes, drain chipset write buffers. */
    int (*publish)(void *, const void *, unsigned, struct intel_wait *);
    uint32_t (*status)(void *, volatile uint32_t *);
    uint64_t (*now)(void *);
    void (*relax)(void *);
    bool (*valid)(void *); /* pinned display/registry generation and scanout */
};
int intel_hic_flush(const struct intel_io *, struct intel_wait *,
                    int (*pause)(void *), void *pause_arg);
struct intel_engine {
    const struct intel_chip *chip;
    struct intel_io io;
    struct fb_device fb;
    struct accel_ops ops;
    uint32_t *memory;
    uint32_t physical, gpu, tail, seq, pgtbl;
    unsigned entries, first;
    uint32_t saved_ptes[INTEL_DMA_PAGES], saved_ring[4], saved_hws;
    bool mapped, touched, active, quarantined, cpu_safe, pending, stopped;
    const char *reason;
    uint64_t operations, waits, fallbacks;
    int (*pause)(void *);
    void *pause_arg;
};
bool intel_mode_valid(const struct fb_device *);
int intel_reserve(struct intel_engine *, const struct fb_device *, const struct intel_io *,
                  uint32_t *, uint32_t, unsigned, uint16_t device);
int intel_engine_start(struct intel_engine *);
int intel_engine_stop(struct intel_engine *);
void intel_engine_quarantine(struct intel_engine *, const char *);
extern const struct display_driver intel2d_gen2_driver, intel2d_gen3_driver;
#endif
