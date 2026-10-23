/* Actual i81x allocation, cache-publication and PCI boundary; no privileged
 * instructions execute in the host fixture. SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include "../../src/kernel/drivers/video/i810/driver.h"
#include <ciuki/cpu.h>
#include <ciuki/mtrr.h>

static struct pci_func functions[4];
static unsigned count, allocated, claimed, freed, phase, reads;
static uint32_t allocation=0x600000, config, selected;
static uint8_t pages[I810_PRIVATE_PAGES*4096u];
static void flush(void) { assert(!phase); phase=1; }
static uint32_t save_irq(void) { return 0x202; }
static void restore_irq(uint32_t flags) { assert(flags==0x202); }
static void config_address(uint16_t port,uint32_t value)
{ assert(port==0xcf8); selected=value; }
static uint32_t config_data(uint16_t port)
{ assert(port==0xcfc && selected==0x80000070u); reads++; return config; }
#undef P2V
#undef irq_save
#undef irq_restore
#define P2V(physical) ((physical)==allocation ? pages : NULL)
#define irq_save save_irq
#define irq_restore restore_irq
#define wbinvd flush
#define outl config_address
#define inl config_data
#include "../../src/kernel/drivers/video/i810/memory.c"

unsigned pci_count(void) { return count; }
const struct pci_func *pci_get(unsigned i) { assert(i<count); return &functions[i]; }
uint32_t pmm_alloc_contig(uint32_t n)
{ assert(n==I810_PRIVATE_PAGES); allocated++; return allocation; }
void ledger_move(uint32_t physical,uint32_t n,enum mm_owner from,enum mm_owner owner)
{ assert(physical==allocation && n==I810_PRIVATE_PAGES && from==MM_KERNEL && owner==MM_DMA); claimed++; }
void pmm_free(uint32_t physical)
{ assert(physical==allocation+freed*4096u); freed++; }
void framebuffer_write_drain(void) { assert(phase==1); phase=2; }

int main(void)
{
    uint32_t physical=0;
    assert(i810_dma_allocate(&physical)==pages && physical==allocation);
    assert(allocated==1 && claimed==1);
    i810_dma_publish(); assert(phase==2);
    i810_dma_free(physical); assert(freed==I810_PRIVATE_PAGES);
    allocation=0;
    assert(!i810_dma_allocate(&physical) && !physical && allocated==2 && claimed==1);

    static const uint16_t devices[]={0x7121,0x7123,0x7125,0x1132};
    for (unsigned i=0;i<sizeof(devices)/sizeof(devices[0]);i++) {
        struct pci_func gpu={.vendor=0x8086,.device=devices[i],.class_code=3};
        functions[0]=(struct pci_func){.vendor=0x8086,
            .device=devices[i]==0x1132 ? 0x1130 : devices[i]-1,.class_code=6};
        count=1;
        config=0; assert(i810_aperture_bytes(&gpu)==64u<<20);
        config=0x10000; assert(i810_aperture_bytes(&gpu)==32u<<20);
        config=UINT32_MAX; assert(!i810_aperture_bytes(&gpu));
        unsigned before=reads;
        count=0; assert(!i810_aperture_bytes(&gpu)); count=1;
        functions[0].vendor=0; assert(!i810_aperture_bytes(&gpu)); functions[0].vendor=0x8086;
        functions[0].class_code=3; assert(!i810_aperture_bytes(&gpu)); functions[0].class_code=6;
        functions[0].fn=1; assert(!i810_aperture_bytes(&gpu)); functions[0].fn=0;
        functions[0].dev=1; assert(!i810_aperture_bytes(&gpu)); functions[0].dev=0;
        functions[1]=functions[0]; count=2;
        assert(!i810_aperture_bytes(&gpu) && reads==before);
    }
    puts("i810 memory boundary: MM_DMA allocation/free, ordered WB/WC publication, exact bridge pairs, aperture decode and refusal PASS");
    return 0;
}
