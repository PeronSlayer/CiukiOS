/* Kernel-private removable floppy service. No userspace ABI.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_FLOPPY_H
#define CIUKI_FLOPPY_H
#include "../fs/blkdev.h"
#include <ciuki/registry.h>
#define FDC_DRIVES 2u
#define FDC_ATTEMPTS 3u
#define FDC_NATIVE UINT32_MAX
struct fdc_geometry {
    uint16_t cylinders;
    uint8_t heads, sectors, rate, gap, format_gap, type;
    bool perpendicular;
};
struct fdc_evidence {
    uint16_t equipment, base;
    uint8_t cmos, irq, dma;
    bool pnp, routed;
};
struct fdc_io {
    uint8_t (*read)(void *, uint16_t);
    void (*write)(void *, uint16_t, uint8_t);
    uint64_t (*now)(void *);
    void (*pause)(void *, unsigned ms);
    uint32_t (*exclude)(void *);
    void (*restore)(void *, uint32_t);
    void (*irq)(void *, bool enable);
    bool (*cancelled)(void *);
    void *ctx;
};
struct fdc;
struct fdc_lease {
    struct fdc *controller;
    uint32_t owner, owner_generation, serial;
    unsigned drive;
};
struct fdc_drive {
    struct fdc *controller;
    struct blkdev block;
    struct fdc_geometry geometry;
    struct fdc_lease native;
    uint32_t owner, owner_generation, serial;
    uint8_t type, cylinder;
    bool present, calibrated, changed, change_high, established;
};
struct fdc {
    struct fdc_io io;
    struct fdc_evidence evidence;
    struct fdc_drive drive[FDC_DRIVES];
    int claims[6];
    gen_t generations[6];
    unsigned claim_count;
    uint8_t *bounce;
    uint32_t physical;
    const struct mm_pin *pin;
    uint64_t deadline, motor_until;
    volatile uint32_t interrupts;
    uint32_t serial, retries;
    uint8_t dor, result[7];
    bool busy, ready, quarantined, dma_armed, touched, irq_attached;
    const char *reason;
};
bool fdc_geometry(unsigned type, struct fdc_geometry *);
int fdc_discover(struct fdc_evidence *, uint16_t equipment, uint8_t cmos);
int fdc_pnp(struct fdc_evidence *, const char *id, uint64_t base,
            uint64_t length, unsigned irq, unsigned dma);
/* Buffer and token remain pinned until detach returns success. */
int fdc_attach(struct fdc *, const struct fdc_io *, const struct fdc_evidence *,
               void *bounce, uint32_t physical, const struct mm_pin *);
int fdc_detach(struct fdc *);
void fdc_irq(struct fdc *);
void fdc_idle(struct fdc *); /* task-context motor timeout, no client I/O */
int fdc_acquire(struct fdc *, unsigned drive, uint32_t owner,
                uint32_t generation, struct fdc_lease *);
int fdc_release(struct fdc_lease *);
int fdc_revalidate(struct fdc_lease *, unsigned media_type);
int fdc_check(struct fdc_lease *);
int fdc_transfer(struct fdc_lease *, uint64_t lba, unsigned count, void *, bool write);
int fdc_format(struct fdc_lease *, unsigned cylinder, unsigned head, uint8_t fill);
/* Native FAT12 adapter: mount while held; unmount before close. */
int fdc_native_open(struct fdc *, unsigned drive, struct blkdev **);
int fdc_native_close(struct fdc *, unsigned drive);
void floppy_init(void);
struct fdc *floppy_controller(void);
const struct fdc *floppy_inventory(void); /* retained facts only, no I/O */
int floppy_native_open(unsigned drive, struct blkdev **);
int floppy_native_close(unsigned drive);
int floppy_mount(unsigned drive);
int floppy_unmount(unsigned drive, bool surprise);
#endif
