/* Shared process/file host allocator failure boundaries. No guest execution.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main process_limits_fixture_main
#include "proc_test.c"
#undef main

int main(void)
{
    ram=calloc(HOST_PAGES,PAGE_SIZE); CHECK(ram);
    CHECK(pmm_total_usable()==HOST_PAGES-PMM_DMA_START_PFN && pmm_free_count()==pmm_total_usable());
    CHECK(!host_alloc_contig(0) && !host_alloc_contig(UINT32_MAX));
    uint32_t p=pmm_alloc(); CHECK(p==PMM_NORMAL_START_PFN*PAGE_SIZE); pmm_free(p);
    CHECK(!host_alloc_run_zone(ZONE_LOW,1,1));
    CHECK(!host_alloc_run_zone(ZONE_DMA,1,0) && !host_alloc_run_zone(ZONE_DMA,1,3));
    p=host_alloc_run_zone(ZONE_DMA,8,8);
    CHECK(p>=PMM_DMA_START_PFN*PAGE_SIZE && p+8*PAGE_SIZE<=PMM_NORMAL_START_PFN*PAGE_SIZE && !(p%(8*PAGE_SIZE)));
    for (unsigned i=0;i<8;i++) pmm_free(p+i*PAGE_SIZE);
    uint32_t run=host_alloc_contig(3); CHECK(run==PMM_NORMAL_START_PFN*PAGE_SIZE);
    for (unsigned i=0;i<3;i++) pmm_free(run+i*PAGE_SIZE);
    /* Exhaust NORMAL: DMA fallback is possible, LOW is never usable. */
    for (unsigned i=PMM_NORMAL_START_PFN;i<HOST_PAGES;i++) allocated[i]=true;
    p=pmm_alloc(); CHECK(p==PMM_DMA_START_PFN*PAGE_SIZE); pmm_free(p);
    /* Only the two adjacent pages across the DMA/NORMAL boundary are
     * free. A naive host scan incorrectly assembles a run of two. */
    for (unsigned i=PMM_DMA_START_PFN;i<HOST_PAGES;i++) allocated[i]=true;
    allocated[PMM_NORMAL_START_PFN-1]=allocated[PMM_NORMAL_START_PFN]=false;
    CHECK(!host_alloc_contig(2));
    p=host_alloc_contig(1); CHECK(p==PMM_NORMAL_START_PFN*PAGE_SIZE); pmm_free(p);
    allocated[PMM_NORMAL_START_PFN]=true;
    p=pmm_alloc(); CHECK(p==(PMM_NORMAL_START_PFN-1)*PAGE_SIZE); pmm_free(p);
    allocated[PMM_NORMAL_START_PFN-1]=true;
    CHECK(!pmm_alloc() && !host_alloc_contig(1));
    /* Fragmentation: ample free pages cannot satisfy a contiguous pair. */
    for (unsigned i=PMM_DMA_START_PFN;i<HOST_PAGES;i++) allocated[i]=!!(i&1);
    CHECK(!host_alloc_contig(2));
    CHECK(!pages_used && !heap_blocks);
    free(ram);
    puts("PASS host limits: LOW reserved, zone order/edges, exhaustion, fragmentation");
    return 0;
}
