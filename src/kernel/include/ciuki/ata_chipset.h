/* Private boot-time ATA negotiation v1. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_ATA_CHIPSET_H
#define CIUKI_ATA_CHIPSET_H
#include <ciuki/registry.h>
#define ATA_CHIPSET_ABI 1u
#define ATA_TIMING_WRITES 12u
enum ata_family { ATA_CHIP_NONE, ATA_CHIP_PIIX, ATA_CHIP_VIA, ATA_CHIP_SIS, ATA_CHIP_ALI, ATA_CHIP_NV,
    ATA_CHIP_PDC_OLD, ATA_CHIP_PDC_NEW, ATA_CHIP_HPT36, ATA_CHIP_HPT37, ATA_CHIP_HPTN };
struct ata_config_ops {
    uint8_t (*read)(void *, const struct pci_func *, unsigned);
    void (*write)(void *, const struct pci_func *, unsigned, uint8_t);
};
struct ata_chipset {
    const struct pci_func *pci, *bridge;
    const struct ata_config_ops *ops;
    void *ctx;
    enum ata_family family;
    unsigned channel, layout;
    int maximum;
    bool cable80, no_lba48_dma;
    uint8_t saved_udma[2];
    const char *reason;
    unsigned clock_mhz;
    unsigned pci_mhz;
    bool prepared;
};
struct ata_timing_write { uint8_t reg, mask, value; };
struct ata_timing_plan { struct ata_timing_write writes[ATA_TIMING_WRITES]; unsigned count; };
struct ata_resources { uint16_t base, control, bm; unsigned irq; bool native_irq; uint32_t mmio; };
struct ata_channel;
struct ata_device;
int ata_pci_resources(const struct pci_func *, unsigned, bool routed, struct ata_resources *);
void ata_chipset_init(struct ata_chipset *, const struct pci_func *, unsigned,
                      const struct ata_config_ops *, void *);
const struct pci_func *ata_chip_bridge(const struct ata_chipset *, uint16_t, uint16_t);
unsigned ata_chip_read(const struct ata_chipset *, unsigned, unsigned);
bool ata_plan_add(struct ata_timing_plan *, unsigned, unsigned, unsigned);
int ata_chipset_plan(struct ata_chipset *, unsigned, const uint8_t[512], struct ata_timing_plan *);
int ata_chipset_program(struct ata_channel *, const struct ata_timing_plan *);
extern const struct ata_config_ops ata_native_config;
/* Offsets are relative to BAR4 I/O or BAR5 MMIO, with donor access widths. */
struct ata_addin_ops {
    uint32_t (*read)(void *, unsigned, unsigned);
    void (*write)(void *, unsigned, unsigned, uint32_t);
};
bool ata_addin_match(const struct pci_func *);
unsigned ata_addin_channels(const struct pci_func *);
int ata_addin_resources(const struct pci_func *, unsigned, bool, struct ata_resources *);
void ata_addin_detect(struct ata_chipset *);
bool ata_addin_plan(struct ata_chipset *, unsigned, unsigned, struct ata_timing_plan *);
int ata_addin_prepare(struct ata_channel *);
int ata_addin_post_mode(struct ata_device *);
int ata_addin_dma_begin(struct ata_device *, uint32_t, bool);
bool ata_addin_dma_end(struct ata_channel *);
bool ata_addin_irq_pending(struct ata_channel *);
void ata_addin_dma_stopping(struct ata_channel *);
int ata_addin_limit(struct ata_chipset *, unsigned, const uint8_t *, int);
extern const struct ata_addin_ops ata_addin_native_ops;
uint32_t ata_addin_read(struct ata_channel *, unsigned, unsigned);
int ata_addin_write(struct ata_channel *, unsigned, unsigned, uint32_t);
int ata_addin_cfg(struct ata_chipset *, unsigned, unsigned, uint32_t);
int ata_addin_wait(struct ata_channel *, unsigned);
bool ata_addin_idle(struct ata_channel *);
int ata_promise_prepare(struct ata_channel *);
int ata_highpoint_prepare(struct ata_channel *);
unsigned ata_raid_locations(uint64_t, uint64_t *, unsigned);
const char *ata_raid_signature(const uint8_t[512]);
void ata_piix_detect(struct ata_chipset *);
void ata_via_detect(struct ata_chipset *);
void ata_sis_detect(struct ata_chipset *);
void ata_ali_detect(struct ata_chipset *);
void ata_nv_detect(struct ata_chipset *);
bool ata_piix_plan(struct ata_chipset *, unsigned, unsigned, struct ata_timing_plan *);
bool ata_via_plan(struct ata_chipset *, unsigned, unsigned, struct ata_timing_plan *);
bool ata_sis_plan(struct ata_chipset *, unsigned, unsigned, struct ata_timing_plan *);
bool ata_ali_plan(struct ata_chipset *, unsigned, unsigned, struct ata_timing_plan *);
bool ata_nv_plan(struct ata_chipset *, unsigned, unsigned, struct ata_timing_plan *);
#endif
