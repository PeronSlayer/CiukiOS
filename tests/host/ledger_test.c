/* Production memory paths on fake physical RAM. No privileged instructions.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ciuki/process.h>
#define CIUKI_CPU_H
#define KERNEL_VBASE 0xC0000000u
static uint8_t *ram;
#define P2V(p) ((void *)(ram+(p)))
#define V2P(p) (0x100000u+(uint32_t)((uintptr_t)(p)&0u))
static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"FAIL ledger:%d %s\n",__LINE__,#x); exit(1); } } while (0)
static uint32_t flags=0x200, cr3;
static uint32_t irq_save(void) { uint32_t f=flags; flags=0; return f; }
static void irq_restore(uint32_t f) { flags=f; }
static uint32_t read_cr3(void) { return cr3; }
static void write_cr3(uint32_t p) { cr3=p; }
static void invlpg(uint32_t p) { (void)p; }
struct ciuki_boot_info g_boot;
struct task *g_current;
volatile uint64_t g_ticks;
char __kernel_start[1],__kernel_end[1],__text_start[1],__rodata_end[1];
static bool expected_panic;
static jmp_buf panic_return;
static unsigned mismatch_records, pressure_records, profile_records;
static unsigned context_records;
static char context_record[256];
void panic(const char *fmt, ...) { (void)fmt; if (expected_panic) longjmp(panic_return,1); abort(); }
void klog(const char *fmt, ...)
{
    char line[256]; va_list ap; va_start(ap,fmt);
    int n=vsnprintf(line,sizeof(line),fmt,ap); va_end(ap); CHECK(n>=0 && n<255);
    if (strstr(fmt,"[mem] ledger mismatch")) mismatch_records++;
    else if (strstr(fmt,"[mem] pressure")) pressure_records++;
    else if (strstr(fmt,"[mem] ledger profile")) profile_records++;
    else if (strstr(fmt,"[mem] pinned_context violation")) {
        context_records++; memcpy(context_record,line,(size_t)n+1);
    }
}
void *kzalloc(size_t bytes) { return calloc(1,bytes); }
void *kmalloc(size_t bytes) { return malloc(bytes); }
void kfree(void *p) { free(p); }
void kpreempt_point(void) { CHECK(flags==0x200); }
void ww_cancel(void *identity,uint64_t generation,uint32_t base,uint32_t end)
{ (void)identity;(void)generation;(void)base;(void)end; }
#include "../../src/kernel/core/pmm.c"
#include "../../src/kernel/core/ledger.c"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpointer-to-int-cast"
#include "../../src/kernel/core/vmm.c"
#pragma clang diagnostic pop
#include "../../src/kernel/proc/uaddr.c"

static uint32_t host_pd[1024];
static void fresh(unsigned mib)
{
    if (pmm_total_usable()) CHECK(ledger_reconcile());
    memset(&g_boot,0,sizeof(g_boot));
    g_boot.e820_count=2;
    g_boot.e820[0]=(struct ciuki_e820){.base=0,.length=0xa0000,.type=CBI_E820_RAM};
    g_boot.e820[1]=(struct ciuki_e820){.base=0x100000,.length=(mib-1u)<<20,.type=CBI_E820_RAM};
    g_boot.kernel_start=0x100000; g_boot.kernel_end=0x300000;
    pmm_init(); kpd=host_pd; cr3=0;
    CHECK(ledger_reconcile());
}
static struct mm_ledger snapshot(void) { struct mm_ledger v; ledger_snapshot(&v); return v; }
static uint32_t resident(enum mm_owner owner)
{ struct mm_ledger v=snapshot(); return v.zone[0].resident[owner]+v.zone[1].resident[owner]+v.zone[2].resident[owner]; }
static void release_run(uint32_t phys,uint32_t pages)
{ for (uint32_t i=0;i<pages;i++) pmm_free(phys+i*PAGE_SIZE); CHECK(ledger_reconcile()); }
static void profiles(void)
{
    const unsigned sizes[]={128,256,512};
    for (unsigned i=0;i<3;i++) {
        fresh(sizes[i]); struct mm_ledger v=snapshot();
        uint32_t u=(sizes[i]-16)*256, low=u/32>1024 ? u/32 : 1024;
        CHECK(v.zone[ZONE_NORMAL].usable==u && v.low==low && v.high==2*low);
        CHECK(v.zone[0].reserved==160 && resident(MM_KERNEL_IMAGE)==512);
        CHECK(!v.commit_pagefile && !v.swap_capacity && !v.swap_used && !v.pressure);
        printf("PASS ledger profile=%u NORMAL=%u low=%u high=%u DMA_free=%u reconciled=1\n",
            sizes[i],u,low,2*low,v.zone[ZONE_DMA].free);
        /* Exhaust both allocatable zones, including NORMAL->DMA fallback. */
        uint32_t n=pmm_alloc_contig(u), dma=pmm_zone_free(ZONE_DMA), d=pmm_alloc_dma_run(dma,1);
        CHECK(n && d && !pmm_alloc() && ledger_pressure()); CHECK(ledger_reconcile());
        CHECK(!pmm_alloc_contig(2) && !pmm_alloc_zone(ZONE_LOW));
        release_run(n,u); release_run(d,dma); CHECK(!ledger_pressure());
        CHECK(snapshot().zone[ZONE_DMA].free==v.zone[ZONE_DMA].free);
    }
}
static void aliases_and_rollback(void);
static uint32_t shared_phys, shared_refs;
static void retain(void *p) { (void)p; shared_refs++; }
static void drop(void *p) { (void)p; CHECK(shared_refs); shared_refs--; }
static uint32_t shared_page(void *p,uint32_t i) { (void)p; CHECK(!i); return shared_phys; }
static void aliases_and_rollback(void)
{
    fresh(64); uint32_t baseline=pmm_free_count();
    struct uaddr u; CHECK(!ua_init(&u,&u));
    shared_phys=pmm_alloc(); mm_claim(shared_phys,1,MM_USER_SHARED);
    struct ua_shared shared={.object=&shared_phys,.retain=retain,.release=drop,.page=shared_page};
    int32_t a=ua_map_shared(&u,PAGE_SIZE,PROT_READ,PROT_READ,&shared);
    int32_t b=ua_map_shared(&u,PAGE_SIZE,PROT_READ,PROT_READ,&shared);
    CHECK(a>0 && b>0 && a!=b && shared_refs==2 && resident(MM_USER_SHARED)==1);
    struct mm_ledger v=snapshot(); CHECK(v.shared_mappings==2 && v.mappings==2);
    CHECK(ledger_reconcile());
    CHECK(!ua_munmap(&u,a,PAGE_SIZE) && resident(MM_USER_SHARED)==1);
    CHECK(!ua_munmap(&u,b,PAGE_SIZE) && !shared_refs);
    mm_free(shared_phys,MM_USER_SHARED);
    /* Leave one page free: table creation succeeds, backing admission fails. */
    uint32_t normal=pmm_zone_free(ZONE_NORMAL), dma=pmm_zone_free(ZONE_DMA);
    uint32_t n=pmm_alloc_contig(normal), d=pmm_alloc_dma_run(dma-1,1);
    CHECK(n && d && pmm_free_count()==1);
    struct mm_ledger before=snapshot();
    CHECK(ua_map_at(&u,CIUKI_MMAP_BASE,3*PAGE_SIZE,PROT_READ|PROT_WRITE,
        PROT_READ|PROT_WRITE,UA_ANON,0)==-ENOMEM);
    CHECK(!u.head && pmm_free_count()==1 && !u.as.tables);
    v=snapshot(); CHECK(v.commit_ram[MM_USER_PRIVATE]==before.commit_ram[MM_USER_PRIVATE]);
    CHECK(v.reserved_va[MM_USER_PRIVATE]==before.reserved_va[MM_USER_PRIVATE]);
    CHECK(ledger_reconcile());
    release_run(n,normal); release_run(d,dma-1); ua_destroy(&u);
    CHECK(pmm_free_count()==baseline && !snapshot().mappings); CHECK(ledger_reconcile());
}
static void pins(void)
{
    fresh(64); uint32_t baseline=pmm_free_count(), p=pmm_alloc_contig(2); mm_claim(p,2,MM_DMA);
    struct mm_pin pin={.generation=17,.first_pfn=p/PAGE_SIZE,.pages=2,.address_mask=UINT32_MAX,
        .alignment=PAGE_SIZE,.boundary=65536,.direction=MM_PIN_FROM_DEVICE};
    CHECK(!mm_pin(&pin) && snapshot().pinned_pages==2); CHECK(ledger_reconcile());
    struct mm_pin stale=pin;
    CHECK(mm_unpin(&pin,18)==-EINVAL && snapshot().pin_tokens==1);
    CHECK(!mm_pin_retire(&pin,17,MM_DMA));
    CHECK(pmm_free_count()==baseline-2 && snapshot().retired_pages==2);
    CHECK(!mm_unpin(&pin,17)); CHECK(ledger_reconcile());
    CHECK(pmm_free_count()==baseline && snapshot().pinned_pages==0);
    CHECK(mm_unpin(&stale,17)==-EINVAL && snapshot().double_unpins==2);
    p=pmm_alloc();
    struct mm_pin next={.generation=18,.first_pfn=p/PAGE_SIZE,.pages=1,.address_mask=UINT32_MAX,
        .alignment=PAGE_SIZE,.direction=MM_PIN_CPU};
    CHECK(!mm_pin(&next) && next.ticket!=stale.ticket);
    CHECK(mm_unpin(&stale,17)==-EINVAL && ledger_page_pinned(p/PAGE_SIZE));
    CHECK(!mm_unpin(&next,18)); pmm_free(p); CHECK(ledger_reconcile());
    /* Constraint failure is transactional. */
    p=pmm_alloc(); pin=(struct mm_pin){.generation=19,.first_pfn=p/PAGE_SIZE,.pages=1,
        .address_mask=0xffffff,.alignment=PAGE_SIZE,.direction=MM_PIN_CPU};
    CHECK(mm_pin(&pin)==-EINVAL && !pin.ticket && !snapshot().pin_tokens); pmm_free(p);
    CHECK(ledger_reconcile());
}
static void reporting_and_context(void)
{
    fresh(128); unsigned before=profile_records;
    const char *s="[desktop] stage=first_frame_presented";
    ledger_lifecycle(s,strlen(s)); ledger_poll(4999); CHECK(profile_records==before);
    ledger_poll(5000); CHECK(profile_records==before+1);
    s="[files] job=copy start items=1"; ledger_lifecycle(s,strlen(s));
    uint32_t capture_free=ledger.profile[2].zone[ZONE_NORMAL].free;
    uint32_t temporary=pmm_alloc();
    CHECK(ledger.profile[2].zone[ZONE_NORMAL].free==capture_free);
    CHECK(snapshot().zone[ZONE_NORMAL].free==capture_free-1);
    ledger_poll(5001); pmm_free(temporary);
    CHECK(profile_records==before+2);
    uint32_t p=pmm_alloc_contig(pmm_zone_free(ZONE_NORMAL));
    ledger_poll(6000); CHECK(pressure_records==1); ledger_poll(6500); CHECK(pressure_records==1);
    release_run(p,(128-16)*256); ledger_poll(7000); CHECK(pressure_records==2);
    ledger_poll(30000); CHECK(profile_records==before+3);
    /* Corrupt a count, prove nonfatal protected mismatch, then restore. */
    ledger.value.zone[ZONE_NORMAL].free++;
    CHECK(!ledger_reconcile() && mismatch_records==1 && snapshot().mismatches==1);
    ledger.value.zone[ZONE_NORMAL].free--; CHECK(ledger_reconcile());
}
/* The same production paths must continue in the default build and fail only
 * with the explicit strict option. Catch each failure, including throttled
 * diagnostics, so strictness cannot accidentally depend on the rate limiter. */
static void context_operation(unsigned operation, uintptr_t site)
{
    uint32_t before=pmm_free_count(), bad=snapshot().bad_context, saved_flags=flags;
#ifdef CIUKI_LEDGER_STRICT
    expected_panic=true;
    if (!setjmp(panic_return)) {
#endif
        uint32_t p=0, pages=operation==1 ? 2 : 1;
        switch (operation) {
        case 0: p=pmm_alloc(); break;
        case 1: p=pmm_alloc_contig(pages); break;
        case 2: p=pmm_alloc_dma_run(pages,1); break;
        case 3: p=pmm_alloc_zone(ZONE_NORMAL); break;
        default: ledger_assert_pinned_context(true,site); break;
        }
        if (operation<4) {
            CHECK(p && pmm_free_count()==before-pages);
            release_run(p,pages);
        }
#ifdef CIUKI_LEDGER_STRICT
        CHECK(false);
    }
    expected_panic=false;
#endif
    CHECK(flags==saved_flags && pmm_free_count()==before);
    CHECK(snapshot().bad_context==bad+1 && ledger_reconcile());
}
static void pinned_context(void)
{
    fresh(64); g_ticks=0;
    unsigned records=context_records;
    struct task refill={.name="refill"}, other={.name="other"};
    g_current=&refill;
    ledger_assert_pinned_context(true,0x11111111);
    CHECK(!snapshot().bad_context && context_records==records);
    ledger_critical(true);
    ledger_assert_pinned_context(false,0x11111111);
    g_current=&other; uint32_t p=pmm_alloc(); CHECK(p); pmm_free(p);
    CHECK(!snapshot().bad_context && context_records==records);
    g_current=&refill;
    context_operation(0,0); /* First violation must print even at tick zero. */
    unsigned site=0; int end=0;
    CHECK(context_records==records+1);
    CHECK(sscanf(context_record,"[mem] pinned_context violation task=refill irq=0 site=%8x%n",&site,&end)==1);
    CHECK(site && end==(int)strlen(context_record));
    for (unsigned op=1;op<4;op++) context_operation(op,0);
    context_operation(4,0xc0123456); /* Fault reporting remains nonfatal by default. */
    CHECK(context_records==records+1 && snapshot().bad_context==5);
    g_ticks=999; context_operation(4,0xc0123456);
    CHECK(context_records==records+1);
    g_ticks=1000; context_operation(4,0xc0123456);
    CHECK(context_records==records+2 && !strcmp(context_record,
        "[mem] pinned_context violation task=refill irq=0 site=c0123456"));
    ledger_critical(false);
    /* Nested IRQ violations preserve IF clear and share the global limiter. */
    flags=0; ledger_irq_context(true); ledger_irq_context(true);
    g_ticks=1999; context_operation(0,0);
    CHECK(context_records==records+2);
    g_ticks=2000; context_operation(4,0xc0765432);
    CHECK(context_records==records+3 && !strcmp(context_record,
        "[mem] pinned_context violation task=refill irq=2 site=c0765432"));
    ledger_irq_context(false); ledger_irq_context(false); flags=0x200;
    ledger_assert_pinned_context(true,0x11111111);
    CHECK(snapshot().bad_context==9 && context_records==records+3);
    /* No task at early boot, and a tick beyond the 32-bit wrap. */
    g_current=0; ledger_irq_context(true); g_ticks=(1ull<<32)+3000;
    context_operation(4,0xc0000001);
    CHECK(context_records==records+4 && !strcmp(context_record,
        "[mem] pinned_context violation task=boot irq=1 site=c0000001"));
    ledger_irq_context(false);
    CHECK(snapshot().bad_context==10 && ledger_reconcile());
}
static void scattered_pool(void)
{
    fresh(64); uint32_t baseline=pmm_free_count();
    struct aspace as; CHECK(!as_create(&as)); as.owner=MM_VM_POOL;
    uint32_t a=pmm_alloc(), gap=pmm_alloc(), b=pmm_alloc();
    mm_claim(a,1,MM_VM_POOL); mm_claim(b,1,MM_VM_POOL);
    CHECK(!as_map(&as,0x110000,a,PTE_U|PTE_W));
    CHECK(!as_map(&as,0x111000,b,PTE_U|PTE_W));
    const uint32_t *pd=P2V(as.pd_phys);
    struct mm_pin pin={.generation=72,.first_pfn=a/PAGE_SIZE,.pages=2,.address_mask=UINT32_MAX,
        .alignment=PAGE_SIZE,.direction=MM_PIN_CPU,.pfn_table=pd[0]&~4095u,.first_entry=0x110};
    CHECK(!mm_pin(&pin) && snapshot().pinned_pages==3);
    CHECK(ledger_page_pinned(a/PAGE_SIZE) && ledger_page_pinned(b/PAGE_SIZE));
    CHECK(!ledger_page_pinned(gap/PAGE_SIZE));
    CHECK(ledger_page_pinned(pin.pfn_table/PAGE_SIZE));
    CHECK(ledger_reconcile()); CHECK(!mm_unpin(&pin,72));
    as_destroy(&as); pmm_free(gap);
    CHECK(pmm_free_count()==baseline && !snapshot().mappings && ledger_reconcile());
}
int main(void)
{
    ram=calloc(1,64u<<20); CHECK(ram);
    profiles(); aliases_and_rollback(); pins(); scattered_pool(); reporting_and_context(); pinned_context();
    struct mm_ledger maximum=snapshot();
    maximum.mappings=maximum.shared_mappings=maximum.pin_tokens=maximum.pinned_pages=UINT32_MAX;
    maximum.retired_pages=maximum.mismatches=maximum.double_unpins=maximum.bad_context=UINT32_MAX;
    record_snapshot("maximum-vm-created",&maximum,UINT64_MAX);
    printf("PASS resource ledger checks=%u static_data=%zu bytes strict=%u\n",checks,sizeof(ledger),
#ifdef CIUKI_LEDGER_STRICT
        1u
#else
        0u
#endif
    );
    free(ram); return 0;
}
