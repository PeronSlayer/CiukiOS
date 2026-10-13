/* SFF-8038i bus master boundary. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_ATA_DMA_H
#define CIUKI_ATA_DMA_H
#include <ciuki/registry.h>
#define ATA_DMA_BYTES 131072u
#define ATA_DMA_PRDS 3u
#define ATA_BM_ACTIVE 1u
#define ATA_BM_ERROR 2u
#define ATA_BM_IRQ 4u
#define ATA_BM_READ 8u /* disk -> memory (PCI bus master writes) */
struct ata_prd { uint32_t address; uint16_t count, flags; };
struct ata_dma_ops {
    uint8_t (*in8)(void *, uint16_t);
    void (*out8)(void *, uint16_t, uint8_t);
    void (*out32)(void *, uint16_t, uint32_t);
    /* Exact contiguous, cacheable RAM range; writable for disk reads. */
    bool (*physical)(void *, const void *, uint32_t, bool, uint32_t *);
};
struct ata_dma {
    const struct ata_dma_ops *ops;
    void *ctx, *bounce;
    struct ata_prd *prd;
    uint32_t prd_phys, bounce_phys, bytes;
    uint32_t pci_config;             /* command register address for emergency revocation */
    uint16_t base;
    uint8_t direction;
    volatile uint8_t irq_status;
    bool armed, bounced, started;
    int claim;
    gen_t generation;
};
struct ata_channel;
int ata_dma_prd(struct ata_prd *, unsigned capacity, uint32_t phys, uint32_t bytes);
int ata_dma_mode(const uint8_t identify[512]); /* selected UDMA, or -1 */
int ata_dma_restore_mode(int firmware_mode, const uint8_t identify[512]);
uint16_t ata_dma_pci_base(const struct pci_func *, unsigned channel);
int ata_dma_setup(struct ata_channel *, uint16_t base, const struct ata_dma_ops *, void *ctx,
                  struct ata_prd *, uint32_t prd_phys, void *bounce, uint32_t bounce_phys);
void ata_dma_attach(struct ata_channel *, unsigned channel);
int ata_dma_prepare(struct ata_channel *, void *, uint32_t bytes, bool write);
void ata_dma_start(struct ata_channel *);
void ata_dma_irq(struct ata_channel *);
bool ata_dma_stop(struct ata_channel *);
void ata_dma_revoke(struct ata_channel *); /* only after failed stop, thread context */
void ata_dma_finish(struct ata_channel *, void *buffer, bool write, bool success);
#endif
