/* i81x kernel-owned backing and read-only firmware aperture qualification.
 * SPDX-License-Identifier: GPL-2.0-only
 * Register reference: Linux v6.7 drivers/char/agp/{intel-gtt.c,intel-agp.h}.
 * No upstream executable code imported. */
#include "driver.h"
#include <ciuki/cpu.h>
#include <ciuki/mtrr.h>

void *i810_dma_allocate(uint32_t *physical)
{
    *physical=pmm_alloc_contig(I810_PRIVATE_PAGES);
    if (!*physical) return 0;
    mm_claim(*physical,I810_PRIVATE_PAGES,MM_DMA);
    return P2V(*physical);
}
void i810_dma_free(uint32_t physical)
{
    for (unsigned i=0;i<I810_PRIVATE_PAGES;i++) mm_free(physical+i*4096u,MM_DMA);
}
void i810_dma_publish(void)
{
    /* i810's Linux AGP/cache publication uses a global CPU cache flush.
     * This uniprocessor port never substitutes WC drain for WB RAM cleaning. */
    wbinvd();
    framebuffer_write_drain();
}
uint32_t i810_aperture_bytes(const struct pci_func *gpu)
{
    const struct pci_func *bridge=0;
    uint16_t expected=gpu->device==0x1132 ? 0x1130 : gpu->device-1;
    for (unsigned i=0;i<pci_count();i++) {
        const struct pci_func *p=pci_get(i);
        if (p->vendor!=0x8086 || p->device!=expected || p->bus!=gpu->bus ||
            p->dev || p->fn || p->class_code!=6 || p->subclass) continue;
        if (bridge) return 0;
        bridge=p;
    }
    if (!bridge) return 0;
    uint32_t flags=irq_save();
    outl(0xcf8,0x80000000u|(uint32_t)bridge->bus<<16|0x70u);
    uint32_t value=inl(0xcfc);
    irq_restore(flags);
    if (value==UINT32_MAX) return 0;
    return value&0x10000u ? 32u<<20 : 64u<<20;
}
