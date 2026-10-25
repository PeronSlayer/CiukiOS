/* The production physical allocator's contiguous run (f4-04), on a fake E820
 * map: zone order, exact accounting, fragmentation, zone edges, and that the
 * ordinary single-page allocator is unaffected. No privileged execution.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL pmm contig line %d: %s\n", __LINE__, #x); failures++; } } while (0)
static uint32_t test_irq_save(void) { return 0; }
static void test_irq_restore(uint32_t f) { (void)f; }
#undef irq_save
#define irq_save test_irq_save
#define irq_restore test_irq_restore
#undef P2V
static uint8_t descriptor_ram[5u<<20] __attribute__((aligned(4096)));
#define P2V(p) ((void *)(descriptor_ram+((p)-0x1000000u)))
#undef V2P
#define V2P(v) ((uint32_t)0x100000u + (uint32_t)((uintptr_t)(v) & 0u))
struct ciuki_boot_info g_boot;
char __kernel_start[1], __kernel_end[1];
void panic(const char *fmt, ...) { (void)fmt; printf("FAIL panic\n"); abort(); }
struct task *g_current;
volatile uint64_t g_ticks;
void klog(const char *fmt, ...) { (void)fmt; }
#include "../../src/kernel/core/pmm.c"
#include "../../src/kernel/core/ledger.c"

static uint32_t zone_normal(void) { return pmm_zone_free(ZONE_NORMAL); }
static uint32_t zone_dma(void) { return pmm_zone_free(ZONE_DMA); }
static void fresh(void)
{
    if (pmm_total_usable()) CHECK(ledger_reconcile());
    memset(&g_boot, 0, sizeof(g_boot));
    g_boot.e820_count = 2;
    g_boot.e820[0] = (struct ciuki_e820){ .base = 0, .length = 0xa0000, .type = CBI_E820_RAM };
    g_boot.e820[1] = (struct ciuki_e820){ .base = 0x100000, .length = 63u << 20, .type = CBI_E820_RAM };  /* to 64 MiB */
    g_boot.kernel_start = 0x100000; g_boot.kernel_end = 0x300000;
    pmm_init();
}
static void release(uint32_t phys, unsigned pages) { for (unsigned i = 0; i < pages; i++) pmm_free(phys + i * PAGE_SIZE); }

int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    fresh();
    uint32_t normal = zone_normal(), dma = zone_dma();
    uint32_t normal_first=4096+descriptor_bytes/PAGE_SIZE;
    CHECK(normal == 12288-descriptor_bytes/PAGE_SIZE && dma > 3000);               /* 16..64 MiB; the kernel image is reserved */
    CHECK(!pmm_alloc_contig(0));
    CHECK(!pmm_alloc_dma_run(0,8) && !pmm_alloc_dma_run(8,0) && !pmm_alloc_dma_run(8,3) && !pmm_alloc_dma_run(3841,8));
    uint32_t isa=pmm_alloc_dma_run(8,8);
    CHECK(isa>=0x300000 && isa+32768<=0x1000000 && !(isa&32767));
    CHECK((isa&0x1ffff)+32768<=0x20000 && zone_dma()==dma-8 && zone_normal()==normal);
    for (unsigned i=0;i<8;i++) CHECK(!pmm_page_free(isa/PAGE_SIZE+i));
    release(isa,8); CHECK(zone_dma()==dma);
    /* There is no NORMAL fallback, even with all of it free. */
    for (unsigned pfn=256;pfn<4096;pfn++) if (!used(pfn)) { set_used(pfn); zone_free[ZONE_DMA]--; ledger_alloc_page(pfn); }
    CHECK(!pmm_alloc_dma_run(8,8) && zone_normal()==normal);
    fresh();

    /* A run comes from the NORMAL zone, aligned, exactly accounted, all pages used. */
    uint32_t run = pmm_alloc_contig(57);
    CHECK(run && !(run & 4095) && run >= (16u << 20) && run + 57 * PAGE_SIZE <= (64u << 20));
    CHECK(zone_normal() == normal - 57 && zone_dma() == dma);
    for (unsigned i = 0; i < 57; i++) CHECK(!pmm_page_free((run >> 12) + i));
    CHECK(pmm_page_free((run >> 12) - 1) || (run >> 12) == normal_first);
    uint32_t next = pmm_alloc_contig(57); CHECK(next == run + 57 * PAGE_SIZE);     /* first fit, no overlap */
    uint32_t single = pmm_alloc(); CHECK(single && single != run && !(single >= run && single < next + 57 * PAGE_SIZE));
    release(single, 1);
    release(run, 57); release(next, 57);
    CHECK(zone_normal() == normal && zone_dma() == dma);
    uint32_t again = pmm_alloc_contig(57); CHECK(again == run);                     /* freed run is reusable */
    release(again, 57);

    /* Too large for the free pages: refused, nothing taken. */
    CHECK(!pmm_alloc_contig(normal + dma + 1) && zone_normal() == normal && zone_dma() == dma);
    uint32_t everything = pmm_alloc_contig(normal);
    CHECK(everything == normal_first*PAGE_SIZE && !zone_normal());                              /* the whole zone, to its last page */
    uint32_t spill = pmm_alloc_contig(4);                                            /* NORMAL exhausted: DMA zone */
    CHECK(spill && spill < (16u << 20) && spill >= (1u << 20) && zone_dma() == dma - 4);
    release(spill, 4); release(everything, normal);
    CHECK(zone_normal() == normal && zone_dma() == dma);

    /* Fragment NORMAL: every other page used. No pair there; DMA serves it. */
    fresh(); normal = zone_normal(); dma = zone_dma();
    uint32_t held[6144]; unsigned count = 0;
    for (unsigned i = 0; i < normal; i += 2) held[count++] = (normal_first + i) * PAGE_SIZE;
    for (unsigned i = 0; i < count; i++) { bitmap[(held[i] >> 12) >> 5] |= 1u << ((held[i] >> 12) & 31); zone_free[ZONE_NORMAL]--; ledger_alloc_page(held[i] >> 12); }
    uint32_t pair = pmm_alloc_contig(2);
    CHECK(pair && pair < (16u << 20) && zone_normal() == normal - count);
    release(pair, 2);
    uint32_t one = pmm_alloc_contig(1); CHECK(one >= (16u << 20));              /* a single page is a run of one */
    release(one, 1);
    /* Fragment DMA as well: nothing contiguous remains. */
    for (unsigned pfn = 256; pfn < 4096; pfn += 2) if (!used(pfn)) { set_used(pfn); zone_free[ZONE_DMA]--; ledger_alloc_page(pfn); }
    CHECK(!pmm_alloc_contig(2) && pmm_alloc_contig(1));

    /* A run never straddles the DMA/NORMAL boundary (pfn 4096). */
    fresh();
    for (unsigned pfn = 256; pfn < 4096 - 3; pfn++) if (!used(pfn)) { set_used(pfn); zone_free[ZONE_DMA]--; ledger_alloc_page(pfn); }
    for (unsigned pfn = normal_first; pfn < normal_first + 3; pfn++) if (!used(pfn)) { set_used(pfn); zone_free[ZONE_NORMAL]--; ledger_alloc_page(pfn); }
    /* Free: DMA pfns 4093..4095 and NORMAL from 4099. NORMAL is searched first. */
    uint32_t edge = pmm_alloc_contig(3); CHECK(edge == (normal_first+3u) * PAGE_SIZE);
    release(edge, 3);
    for (unsigned pfn = normal_first + 3; pfn < 4096 + 12288; pfn++) if (!used(pfn)) { set_used(pfn); zone_free[ZONE_NORMAL]--; ledger_alloc_page(pfn); }
    uint32_t over = pmm_alloc_contig(4); CHECK(!over);                               /* 3 pages left below the boundary */
    uint32_t last = pmm_alloc_contig(3); CHECK(last == 4093u * PAGE_SIZE);

    CHECK(ledger_reconcile());
    printf("%s pmm_alloc_contig: zones, accounting, fragmentation, edges checks=%u failures=%u\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
