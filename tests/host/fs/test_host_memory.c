/* Filesystem host pages must fail for target-zone constraints, not only OOM.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "fs_port.h"
#include <ciuki/memory_limits.h>
#include <assert.h>
#include <stdio.h>
int main(void)
{
    fs_host_memory_configure(4,4);
    void *page[8];
    for (unsigned i=0;i<8;i++) {
        if (i==4) assert(!fs_normal_page_alloc()); /* DMA is still free */
        page[i]=fs_page_alloc(); assert(page[i] && !((uintptr_t)page[i]&(PAGE_SIZE-1)));
        unsigned pfn=(i<4 ? PMM_NORMAL_START_PFN+i : PMM_DMA_START_PFN+i-4);
        assert(fs_host_memory_pfn(page[i])==pfn);
        for (unsigned b=0;b<PAGE_SIZE;b++) assert(!((uint8_t *)page[i])[b]);
    }
    assert(fs_host_memory_live()==8 && !fs_page_alloc());
    for (unsigned i=1;i<8;i+=2) fs_page_free(page[i]);
    assert(fs_host_memory_live()==4 && !fs_contig_alloc(2*PAGE_SIZE));
    fs_page_free(page[2]);
    void *run=fs_contig_alloc(3*PAGE_SIZE);
    assert(run && fs_host_memory_pfn(run)==PMM_NORMAL_START_PFN+1 && fs_host_contig_live==1);
    fs_contig_free(run,3*PAGE_SIZE);
    fs_page_free(page[0]); fs_page_free(page[4]); fs_page_free(page[6]);
    assert(!fs_host_memory_live() && !fs_host_contig_live);
    fs_host_memory_configure(1,1);
    assert(!fs_contig_alloc(PAGE_SIZE+1)); /* cannot join zones */
    assert(!fs_contig_alloc(0) && !fs_contig_alloc(SIZE_MAX));
    fs_host_page_fail=1; assert(!fs_page_alloc());
    fs_host_contig_fail=1; assert(!fs_contig_alloc(1));
    run=fs_contig_alloc(1); assert(run && fs_host_memory_live()==1);
    fs_contig_free(run,1); assert(!fs_host_memory_live());
    fs_host_memory_configure(PMM_MAX_PAGES-PMM_NORMAL_START_PFN,PMM_NORMAL_START_PFN-PMM_DMA_START_PFN);
    puts("PASS filesystem host pages: zones, alignment/zeroing, exhaustion, fragmentation, no cross-zone run, overflow, fault injection, cleanup");
    return 0;
}
