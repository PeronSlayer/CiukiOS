/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_LPT_H
#define CIUKI_LPT_H
#include <ciuki/abi.h>
#include <ciuki/registry.h>
struct file_description;
struct process;
/* Assigned extents only. capabilities are resource/configuration facts, never
 * inferred from a PCI class or an unclaimed high address. dma=NO_DMA if absent. */
struct lpt_resource {
    uint32_t number, base, length, base_hi, irq, dma, origin, capabilities;
};
struct lpt_io {
    uint8_t (*read)(void *, uint16_t);
    void (*write)(void *, uint16_t, uint8_t);
    void (*sleep)(void *);
    int (*delay)(void *, unsigned);
    void *context;
};
void lpt_init(void);
int lpt_admit(const struct lpt_resource *, const struct lpt_io *);
void lpt_bda_discover(const uint16_t bases[3]);
int lpt_pnp_assigned(const char *, uint32_t base, uint32_t length, uint32_t high,
                     unsigned irq, unsigned dma);
void lpt_pci_discover(const struct pci_func *);
bool lpt_snapshot(unsigned, struct ciuki_lpt_status *, const char **);
int lpt_open(unsigned, uint32_t owner, uint32_t owner_generation, bool raw, uint32_t *);
void lpt_close(unsigned, uint32_t);
void lpt_abort(unsigned, uint32_t);
int lpt_transfer(unsigned, uint32_t, void *, unsigned, bool write, bool nonblock,
                 bool (*cancelled)(void *), void *cancel_arg);
int lpt_control(unsigned, uint32_t, uint32_t, void *, unsigned, bool writable);
int lpt_raw_io(unsigned, uint32_t, unsigned reg, bool write, uint8_t *);
int lpt_device_open(struct file_description *, uint32_t);
void lpt_device_close(struct file_description *);
int lpt_rw_user(struct file_description *, uint32_t, uint32_t, bool);
int lpt_devctl(struct process *, int32_t, uint32_t, uint32_t, uint32_t);
#endif
