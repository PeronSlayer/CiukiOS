/* Private 82077-compatible VM frontend, no physical port access.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_VIRTUAL_FLOPPY_H
#define CIUKI_VIRTUAL_FLOPPY_H
#include "../drivers/floppy.h"
struct vfdc_ops {
    int (*check)(void *, unsigned);
    int (*sector)(void *, unsigned, uint32_t, void *, bool);
    int (*format)(void *, unsigned, unsigned, unsigned, uint8_t);
    int (*dma)(void *, void *, unsigned, bool to_guest);
    unsigned (*dma_bytes)(void *, bool to_guest);
    void (*irq)(void *);
    void *ctx;
    int (*revalidate)(void *, unsigned);
    bool (*status)(void *, unsigned, uint8_t result[3]);
};
struct vfdc {
    struct vfdc_ops ops;
    struct fdc_geometry geometry[2];
    uint8_t command[16], result[16], sense[8], cylinder[2], sector[512];
    uint8_t dor, rate, length, needed, position, results, senses;
    bool present[2], changed[2], executing;
};
void vfdc_init(struct vfdc *, const struct vfdc_ops *);
uint8_t vfdc_io(struct vfdc *, unsigned reg, bool write, uint8_t);
uint8_t floppy_bios_status(int error);
#endif
