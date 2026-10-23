/* Private boot-time ATA negotiation v1. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_ATA_CHIPSET_H
#define CIUKI_ATA_CHIPSET_H
#include <ciuki/registry.h>
#define ATA_CHIPSET_ABI 1u
#define ATA_TIMING_WRITES 12u
enum ata_family { ATA_CHIP_NONE, ATA_CHIP_PIIX, ATA_CHIP_VIA, ATA_CHIP_SIS, ATA_CHIP_ALI, ATA_CHIP_NV };
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
};
struct ata_timing_write { uint8_t reg, mask, value; };
struct ata_timing_plan { struct ata_timing_write writes[ATA_TIMING_WRITES]; unsigned count; };
struct ata_resources { uint16_t base, control, bm; unsigned irq; bool native_irq; };
struct ata_channel;
int ata_pci_resources(const struct pci_func *, unsigned, bool routed, struct ata_resources *);
void ata_chipset_init(struct ata_chipset *, const struct pci_func *, unsigned,
                      const struct ata_config_ops *, void *);
const struct pci_func *ata_chip_bridge(const struct ata_chipset *, uint16_t, uint16_t);
unsigned ata_chip_read(const struct ata_chipset *, unsigned, unsigned);
bool ata_plan_add(struct ata_timing_plan *, unsigned, unsigned, unsigned);
int ata_chipset_plan(struct ata_chipset *, unsigned, const uint8_t[512], struct ata_timing_plan *);
int ata_chipset_program(struct ata_channel *, const struct ata_timing_plan *);
extern const struct ata_config_ops ata_native_config;
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
