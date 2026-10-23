/* Native glue, SPDX-License-Identifier: GPL-2.0-only. Private driver API. */
#ifndef CIUKI_UNICHROME_ENGINE_H
#define CIUKI_UNICHROME_ENGINE_H
#include <ciuki/display_driver.h>

#define UC_MMIO_BYTES 0x10000u
#define UC_WAIT_US 20000u
#define UC_POLL_LIMIT 65536u
#define UC_SCRATCH_BYTES 4096u
struct uc_io {
    void *ctx;
    uint32_t (*read)(void *, unsigned);
    void (*write)(void *, unsigned, uint32_t);
    uint8_t (*indexed)(void *, bool, uint8_t);
    uint64_t (*now)(void *);
    void (*drain)(void *);
    bool (*valid)(void *);
    void (*indexed_write)(void *, bool, uint8_t, uint8_t);
};
struct uc_engine {
    struct uc_io io;
    const struct fb_device *fb;
    struct accel_ops ops;
    uint8_t *scratch;
    uint32_t scratch_offset, status, saved[17], generation;
    uint64_t operations, timeouts;
    bool active, quarantined, cpu_safe, saved_valid, touched, verified_write;
    bool transport_saved, mmio_changed;
    uint8_t saved_sr10, saved_sr1a;
    const char *reason;
    int (*pause)(void *);
    void *pause_ctx;
};
int uc_engine_init(struct uc_engine *, const struct fb_device *, const struct uc_io *,
                   uint8_t *, uint32_t);
void uc_engine_quarantine(struct uc_engine *, const char *);
int uc_engine_probe(void *);
bool uc_pci_supported(const struct pci_func *);
extern const struct display_driver unichrome_driver;
#endif
