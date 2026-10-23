/* Ciuki native Matrox adapter interfaces; private to this driver and fixtures.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_MATROX_H
#define CIUKI_MATROX_H
#include <ciuki/display_driver.h>
#include "registers.h"

struct matrox_io {
    void *ctx;
    uint32_t (*read32)(void *, unsigned);
    void (*write32)(void *, unsigned, uint32_t);
    uint8_t (*read8)(void *, unsigned);
    void (*write8)(void *, unsigned, uint8_t);
    uint64_t (*now_us)(void *);
    void (*drain)(void *);
};
struct matrox_engine {
    struct matrox_io io;
    struct fb_device fb;
    struct accel_ops ops;
    uint32_t pitch, maccess, cursor_offset;
    volatile uint8_t *cursor;
    bool active, quarantined, cpu_safe, touched, shape, cursor_hidden;
    const char *reason;
    uint64_t operations, waits, failures;
    int (*pause)(void *);
    void *pause_arg;
};
bool matrox_mode(const struct fb_device *, const struct pci_func *);
bool matrox_probe(const struct pci_func *);
int matrox_engine_init(struct matrox_engine *, const struct fb_device *,
                        const struct matrox_io *, volatile uint8_t *, uint32_t);
void matrox_engine_quarantine(struct matrox_engine *, const char *);
int matrox_engine_prove(struct matrox_engine *);
/* Prepared for part C: opaque MSB-left 1bpp, packed colours, no public bit.
 * Caller stages the full immutable payload before locking; bounded width 256.
 * Refusal is side-effect-free, error after a row is partial execution. */
int matrox_mono(struct matrox_engine *, const struct fb_rect *, const uint8_t *,
                 uint32_t stride, uint32_t size, uint32_t fg, uint32_t bg);
extern const struct display_driver matrox_driver;
#endif
