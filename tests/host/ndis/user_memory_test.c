/* Production address-space and user staging code with fake physical RAM.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "src/kernel/ndis/user_memory.h"
#include <ciuki/kernel.h>
#define CIUKI_CPU_H
#define KERNEL_VBASE 0xc0000000u
static uint8_t ram[1024*1024];
#define P2V(p) ((void *)(ram+(p)))
static uint32_t tables_next,frame_next;
static unsigned freed;
static uint32_t kpd[1024],kpd_phys;
uint32_t g_user_mappings;
static uint32_t *table_va(uint32_t p) { assert(p<sizeof(ram));return P2V(p); }
static uint32_t read_cr3(void) { return 0; }
static void write_cr3(uint32_t p) { (void)p; }
static void invlpg(uint32_t p) { (void)p; }
void kpreempt_point(void) { }
uint32_t pmm_alloc(void) { uint32_t p=tables_next;tables_next+=4096;assert(tables_next<0x40000);return p; }
uint32_t pmm_alloc_contig(uint32_t n) { uint32_t p=frame_next;frame_next+=n*4096;assert(frame_next<sizeof(ram));return p; }
void pmm_free(uint32_t p) { assert(p<sizeof(ram));freed++; }
static struct mm_pin live_pin;
static bool pin_live,quiescent=true;
int mm_pin(struct mm_pin *p) { assert(!pin_live);p->ticket=123;live_pin=*p;pin_live=true;return 0; }
int mm_unpin(struct mm_pin *p,uint32_t gen) { assert(mm_pin_valid(p,gen));pin_live=false;return 0; }
bool mm_pin_valid(const struct mm_pin *p,uint32_t gen)
{ return pin_live && p && p->generation==gen && !memcmp(p,&live_pin,sizeof(*p)); }
bool ledger_page_pinned(uint32_t pfn)
{ return pin_live && pfn>=live_pin.first_pfn && pfn-live_pin.first_pfn<live_pin.pages; }
static bool is_quiescent(void *c) { (void)c;return quiescent; }
static struct resource bar={.start=0xf1000000,.end=0xf1002000,.type=RES_MMIO,.state=RS_ACTIVE};
bool registry_valid(int claim,gen_t gen) { return claim==3 && gen==5 && bar.state==RS_ACTIVE; }
const struct resource *registry_get(unsigned claim) { assert(claim==3);return &bar; }
#include "vmm_user.inc"
#include "src/kernel/ndis/user_memory.c"
static struct aspace fresh(void)
{
    memset(ram,0,sizeof(ram));memset(kpd,0,sizeof(kpd));
    tables_next=0x3000;frame_next=0x80000;freed=g_user_mappings=0;
    pin_live=false;quiescent=true;bar.state=RS_ACTIVE;
    return (struct aspace){.pd_phys=4096,.owner=MM_NET};
}
int main(void)
{
    struct aspace as=fresh();
    struct ndis_user_memory m={.as=&as,.generation=77,.quiescent=is_quiescent,.execution_base=0x100000,.reserved_bytes=16384};
    assert(ndis_user_grant_bar(&m,0,3,5,bar.start,8192));
    assert(!ndis_user_grant_bar(&m,1,3,5,bar.start+1,8192));
    assert(!ndis_user_grant_bar(&m,1,3,6,bar.start,8192));
    assert(!ndis_user_grant_bar(&m,1,3,5,bar.start,4096)); /* no partial claim */
    struct ndis_host_map view=m.grant[0].view;view.va=0x200000;
    assert(ndis_user_mapping_owned(&m,&view));assert(!ndis_user_mapping_apply(&m,&view,true));
    uint32_t pte=0;assert(as_lookup(&as,view.va,&pte)==bar.start);
    assert((pte&(PTE_PCD|PTE_PWT|PTE_BORROWED|PTE_U))==(PTE_PCD|PTE_PWT|PTE_BORROWED|PTE_U));
    assert(as_map(&as,view.va,0x90000,PTE_U)<0);assert(as_replace_pages(&as,view.va,0,1,0,0,0)<0);
    struct aspace ua=as;ua.memory=(void *)1;
    assert(as_map_borrowed(&ua,0x300000,0x80000,PTE_U)<0);
    /* A preexisting borrowed collision rolls back only this transaction. */
    struct ndis_host_map collision=view;collision.va=0x1ff000;
    assert(ndis_user_mapping_apply(&m,&collision,true)<0);
    assert(!as_lookup(&as,0x1ff000,0) && as_lookup(&as,0x200000,0)==bar.start);
    bar.state=RS_QUARANTINED;assert(!ndis_user_mapping_owned(&m,&view));bar.state=RS_ACTIVE;
    bar.state=RS_QUIESCING;assert(!ndis_user_mapping_owned(&m,&view));bar.state=RS_ACTIVE;
    assert(!ndis_user_mapping_apply(&m,&view,false));assert(!g_user_mappings && !freed);
    /* Kernel WB alias blocks a UC grant; matching UC attributes pass. */
    kpd[768]=0x2000|PTE_P|PTE_W;table_va(0x2000)[0]=bar.start|PTE_P|PTE_W;
    assert(!ndis_user_grant_bar(&m,1,3,5,bar.start,8192));
    table_va(0x2000)[0]|=PTE_PCD|PTE_PWT;
    assert(ndis_user_grant_bar(&m,1,3,5,bar.start,8192));
    assert(!vmm_cache_alias_ok(bar.start,8192,0));kpd[768]=0;
    /* Separate staging pointer and execution VA, exact pinned user residency. */
    struct ntimage_platform p;ntimage_user_platform(&p,&m);
    uint32_t base=0;void *image=p.memory.allocate(&m,0xc1000000,8192,&base);
    assert(image==P2V(m.physical) && base==0x100000 && m.mapped==8192);
    assert(p.resident(&m,image,8192,base));
    struct pe_image code={.base=base,.size=8192,.section_count=2,
        .sections={{.rva=0,.span=4096,.flags=0x60000000},{.rva=4096,.span=4096,.flags=0xc0000000}}};
    assert(p.seal_code(&m,&code));
    assert(!as_range_ok(&as,base,1,true) && as_range_ok(&as,base+4096,1,true));
    code.sections[1].rva=4000;assert(!p.seal_code(&m,&code));assert(!p.resident(&m,image,8192,base+4096));
    quiescent=false;assert(!p.resident(&m,image,8192,base));quiescent=true;
    struct mm_pin original=m.image_pin;
    assert(ndis_user_grant_dma(&m,2,&original));
    struct ndis_host_map dma=m.grant[2].view;dma.va=0x300000;
    assert(ndis_user_mapping_owned(&m,&dma));
    live_pin.ticket++;assert(!ndis_user_mapping_owned(&m,&dma));live_pin.ticket--;
    assert(!ndis_user_mapping_apply(&m,&dma,true));
    unsigned before=freed;assert(!ndis_user_mapping_apply(&m,&dma,false));assert(freed==before && pin_live);
    p.memory.release(&m,image,8192);assert(!m.physical && !pin_live && freed==before+2);
    /* Reaping an uncertain alias never frees backing. */
    assert(!as_map_borrowed(&as,0x400000,0xf1000000,PTE_U|PTE_PCD|PTE_PWT));
    before=freed;unsigned tables=as.tables;as_destroy(&as);
    assert(freed==before+tables+1 && !as.pd_phys && !g_user_mappings);
    puts("PASS production borrowed mappings, rollback, cache aliases, staging VA, DMA ticket identity, safe reap");
    fflush(stdout);return 0;
}
