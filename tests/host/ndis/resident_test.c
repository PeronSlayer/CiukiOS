/* SPDX-License-Identifier: GPL-2.0-only
 * Fake PMM/ledger verifies the production resident allocator's ownership,
 * pins, supervisor checks and rollback. No physical pointer is dereferenced. */
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include "src/kernel/ndis/resident.h"
static uint32_t allocated, freed, owned, pinned;
static bool allocation_failure, pin_failure, unpin_failure, user_pte;
uint32_t pmm_alloc_contig(uint32_t pages) {
    if(allocation_failure) return 0;
    assert(!allocated);allocated=pages;return 0x100000;
}
void pmm_free(uint32_t physical) { (void)physical;assert(allocated);allocated--;freed++; }
void pmm_free_owned(uint32_t physical,enum mm_owner owner) {
    assert(owner==MM_NET && owned);owned--;pmm_free(physical);
}
void ledger_move(uint32_t physical,uint32_t pages,enum mm_owner from,enum mm_owner to) {
    (void)physical;
    if(from==MM_KERNEL && to==MM_NET) owned+=pages;
    else { assert(from==MM_NET && to==MM_KERNEL && owned>=pages);owned-=pages; }
}
int mm_pin(struct mm_pin *p) {
    assert(p->direction==MM_PIN_CPU && p->generation && p->alignment==4096);
    if(pin_failure) return -1;
    pinned=p->pages;p->ticket=1;return 0;
}
int mm_unpin(struct mm_pin *p,uint32_t generation) {
    assert(generation==p->generation && p->ticket && pinned);
    if(unpin_failure) return -1;
    pinned=0;p->ticket=0;return 0;
}
bool ledger_page_pinned(uint32_t pfn) { (void)pfn;return pinned!=0; }
uint32_t vmm_kernel_pte(uint32_t va) { (void)va;return PTE_P|PTE_W|(user_pte?PTE_U:0); }
void klog(const char *fmt,...) { (void)fmt; }
int main(void) {
    struct ndis_resident_memory m={.reserved_bytes=65536,.generation=11};
    struct ntimage_platform p;ntimage_kernel_platform(&p,&m);uint32_t base=0;
    void *v=p.memory.allocate(&m,0,8192,&base);
    assert(v && base==0xc0100000 && allocated==2 && owned==2 && pinned==2);
    assert(p.resident(&m,v,8192,base));user_pte=true;assert(!p.resident(&m,v,8192,base));user_pte=false;
    assert(!p.memory.allocate(&m,0,4096,&base)); /* one private image per arena */
    p.memory.release(&m,v,8192);assert(!m.physical && !allocated && !owned && !pinned && freed==2);
    allocation_failure=true;assert(!p.memory.allocate(&m,0,4096,&base));allocation_failure=false;
    pin_failure=true;assert(!p.memory.allocate(&m,0,4096,&base));pin_failure=false;
    assert(!m.physical && !allocated && !owned && !pinned);
    assert(!p.memory.allocate(&m,0,65537,&base));
    assert(!p.memory.allocate(&m,0,NDIS_IMAGE_LIMIT+1,&base));
    m.generation=0;assert(!p.memory.allocate(&m,0,4096,&base));m.generation=11;
    v=p.memory.allocate(&m,0,4096,&base);assert(v);
    unpin_failure=true;p.memory.release(&m,v,4096);assert(m.physical && allocated && owned && pinned);
    unpin_failure=false;p.memory.release(&m,v,4096);assert(!m.physical && !allocated && !owned && !pinned);
    puts("PASS production resident allocator pin ownership supervisor failure rollback");fflush(stdout);return 0;
}
