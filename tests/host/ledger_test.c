/* Production memory paths on fake physical RAM. No privileged instructions.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ciuki/process.h>
#include <ciuki/dosvm.h>
#define CIUKI_CPU_H
#define KERNEL_VBASE 0xC0000000u
static uint8_t *ram;
#define P2V(p) ((void *)(ram+(p)))
#define V2P(p) (0x100000u+(uint32_t)((uintptr_t)(p)&0u))
static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"FAIL ledger:%d %s\n",__LINE__,#x); exit(1); } } while (0)
static uint32_t flags=0x200, cr3;
static void (*memory_irq_restore)(void);
static uint32_t irq_save(void) { uint32_t f=flags; flags=0; return f; }
static void irq_restore(uint32_t f) {
    flags=f;
    if ((f&0x200) && memory_irq_restore) {
        void (*observe)(void)=memory_irq_restore; memory_irq_restore=0;
        observe(); memory_irq_restore=observe;
    }
}
static uint32_t read_cr3(void) { return cr3; }
static unsigned invalidations, reloads;
static void write_cr3(uint32_t p) { cr3=p; reloads++; }
static void invlpg(uint32_t p) { (void)p; invalidations++; }
struct ciuki_boot_info g_boot;
struct task *g_current;
volatile uint64_t g_ticks;
char __kernel_start[1],__kernel_end[1],__text_start[1],__rodata_end[1];
static bool expected_panic;
static jmp_buf panic_return;
static unsigned mismatch_records, pressure_records, profile_records;
static unsigned context_records;
static bool emit_profiles;
static char context_record[256];
void panic(const char *fmt, ...) { if (expected_panic) longjmp(panic_return,1); va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap); abort(); }
void klog(const char *fmt, ...)
{
    char line[256]; va_list ap; va_start(ap,fmt);
    int n=vsnprintf(line,sizeof(line),fmt,ap); va_end(ap); CHECK(n>=0 && n<255);
    if (emit_profiles && strstr(fmt,"[mem] ledger")) puts(line);
    if (strstr(fmt,"[mem] ledger mismatch")) mismatch_records++;
    else if (strstr(fmt,"[mem] pressure")) pressure_records++;
    else if (strstr(fmt,"[mem] ledger profile")) profile_records++;
    else if (strstr(fmt,"[mem] pinned_context violation")) {
        context_records++; memcpy(context_record,line,(size_t)n+1);
    }
}
static int memory_failure=-1;
static bool memory_allocation_fails(void)
{ if (memory_failure<0) return false; if (!memory_failure) return true; memory_failure--; return false; }
void *kmalloc(size_t bytes) { return memory_allocation_fails()?0:malloc(bytes); }
void *kzalloc(size_t bytes) { void *p=kmalloc(bytes); if (p) memset(p,0,bytes); return p; }
void kfree(void *p) { free(p); }
static void (*memory_service)(void), (*memory_sleep)(void);
void kpreempt_point(void) { CHECK(flags==0x200); if (memory_service) memory_service(); }
void task_sleep_ms(uint32_t ms) { CHECK(ms && flags==0x200 && memory_sleep); memory_sleep(); }
void ww_cancel(void *identity,uint64_t generation,uint32_t base,uint32_t end)
{ (void)identity;(void)generation;(void)base;(void)end; }
#define pmm_alloc physical_alloc
#include "../../src/kernel/core/pmm.c"
#undef pmm_alloc
uint32_t pmm_alloc(void) { return memory_allocation_fails()?0:physical_alloc(); }
#include "../../src/kernel/core/ledger.c"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpointer-to-int-cast"
#include "../../src/kernel/core/vmm.c"
#pragma clang diagnostic pop
#include "../../src/kernel/proc/uaddr.c"

static uint32_t host_pd[1024];
static void fresh_kernel(unsigned mib, unsigned kernel_mib)
{
    if (pmm_total_usable()) CHECK(ledger_reconcile());
    memset(&g_boot,0,sizeof(g_boot));
    g_boot.e820_count=2;
    g_boot.e820[0]=(struct ciuki_e820){.base=0,.length=0xa0000,.type=CBI_E820_RAM};
    g_boot.e820[1]=(struct ciuki_e820){.base=0x100000,.length=(mib-1u)<<20,.type=CBI_E820_RAM};
    g_boot.kernel_start=0x100000; g_boot.kernel_end=(1u+kernel_mib)<<20;
    pmm_init(); kpd=host_pd; cr3=0;
    CHECK(ledger_reconcile());
}
static void fresh(unsigned mib) { fresh_kernel(mib,2); }
static struct mm_ledger snapshot(void) { struct mm_ledger v; ledger_snapshot(&v); return v; }
static uint32_t resident(enum mm_owner owner)
{ struct mm_ledger v=snapshot(); return v.zone[0].resident[owner]+v.zone[1].resident[owner]+v.zone[2].resident[owner]; }
static void release_run(uint32_t phys,uint32_t pages)
{ for (uint32_t i=0;i<pages;i++) pmm_free(phys+i*PAGE_SIZE); CHECK(ledger_reconcile()); }
static void profiles(void)
{
    const unsigned sizes[]={128,256,512,768};
    for (unsigned i=0;i<4;i++) {
        fresh(sizes[i]); struct mm_ledger v=snapshot();
        uint32_t u=(sizes[i]-16)*256, low=u/32>1024 ? u/32 : 1024;
        CHECK(v.zone[ZONE_NORMAL].usable==u && v.low==low && v.high==2*low);
        CHECK(v.zone[0].reserved==160 && resident(MM_KERNEL_IMAGE)==512);
        CHECK(!v.commit_pagefile && !v.swap_capacity && !v.swap_used && !v.pressure);
        printf("PASS ledger profile=%u NORMAL=%u low=%u high=%u DMA_free=%u reconciled=1\n",
            sizes[i],u,low,2*low,v.zone[ZONE_DMA].free);
        /* Exhaust both allocatable zones, including NORMAL->DMA fallback. */
        uint32_t available=u-descriptor_bytes/PAGE_SIZE;
        CHECK(descriptor_count==sizes[i]*256 && descriptor_bytes==sizes[i]*6144);
        CHECK(resident(MM_PFN_DESCRIPTORS)==descriptor_bytes/PAGE_SIZE);
        printf("TRACE PFN profile=%u entries=%u bytes=%u NORMAL_reserved=%u\n",sizes[i],descriptor_count,descriptor_bytes,descriptor_bytes/PAGE_SIZE);
        uint32_t n=pmm_alloc_contig(available), dma=pmm_zone_free(ZONE_DMA), d=pmm_alloc_dma_run(dma,1);
        CHECK(n && d && !pmm_alloc() && ledger_pressure()); CHECK(ledger_reconcile());
        CHECK(!pmm_alloc_contig(2) && !pmm_alloc_zone(ZONE_LOW));
        release_run(n,available); release_run(d,dma); CHECK(!ledger_pressure());
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
    CHECK(mm_pin_valid(&pin,17) && !mm_pin_valid(&pin,18));
    struct mm_pin stale=pin;
    struct mm_pin changed=pin;changed.ticket++;
    CHECK(!mm_pin_valid(&changed,17));changed=pin;changed.pages--;
    CHECK(!mm_pin_valid(&changed,17));
    CHECK(mm_unpin(&pin,18)==-EINVAL && snapshot().pin_tokens==1);
    CHECK(!mm_pin_retire(&pin,17,MM_DMA));
    CHECK(!mm_pin_valid(&pin,17));
    CHECK(pmm_free_count()==baseline-2 && snapshot().retired_pages==2);
    CHECK(!mm_unpin(&pin,17)); CHECK(ledger_reconcile());
    CHECK(pmm_free_count()==baseline && snapshot().pinned_pages==0);
    CHECK(mm_unpin(&stale,17)==-EINVAL && snapshot().double_unpins==2);
    p=pmm_alloc();
    struct mm_pin next={.generation=18,.first_pfn=p/PAGE_SIZE,.pages=1,.address_mask=UINT32_MAX,
        .alignment=PAGE_SIZE,.direction=MM_PIN_CPU};
    CHECK(!mm_pin(&next) && next.ticket!=stale.ticket);
    CHECK(!mm_pin_valid(&stale,17) && mm_pin_valid(&next,18));
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
    uint32_t normal=pmm_zone_free(ZONE_NORMAL), p=pmm_alloc_contig(normal);
    ledger_poll(6000); CHECK(pressure_records==1); ledger_poll(6500); CHECK(pressure_records==1);
    release_run(p,normal); ledger_poll(7000); CHECK(pressure_records==2);
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
/* Declared budget model, not measured boot/desktop or physical hardware.
 * Reserve the entire 1..16 MiB kernel load window, then the existing boot
 * targets: heap 4 MiB, tables 1.5 MiB, system cache 8 MiB, desktop 3 MiB.
 * VM PFNs come from production dosvm_create in the other host harness.
 * f3-46 (contract D1): the VM commitment is split as dosvm_pool.c does:
 * the fixed part is a VM_PRIVATE promise, the 24 MiB pool a VM_POOL
 * commitment admitted by ledger_commit (RAM or published pagefile). Two
 * residency states are replayed: created (paged pool not resident) and
 * touched (whole pool resident, also the locked mode's creation state). */
static uint64_t vm_admit_margin(uint32_t free_pages, uint64_t reserved, uint64_t resident)
{
    /* dosvm_admit: request + headroom <= free - (reserved - resident). */
    uint64_t free = (uint64_t)free_pages * PAGE_SIZE, outstanding = reserved - resident;
    uint64_t need = (uint64_t)DOSVM_RESIDENT_CAP + DOSVM_HEADROOM + outstanding;
    return free >= need ? free - need : 0;
}
static void dosvm_profiles(int argc, char **argv)
{
    CHECK(argc == 2 * MM_OWNERS + 7);
    unsigned charges[2 * MM_OWNERS + 5];
    for (unsigned i=0;i<ARRAY_SIZE(charges);i++) {
        char *end; unsigned long value=strtoul(argv[i+2],&end,10);
        CHECK(*argv[i+2] && !*end && value<=UINT32_MAX);
        charges[i]=(unsigned)value;
    }
    const unsigned *created=charges, *touched=charges+MM_OWNERS;
    unsigned cache=charges[2*MM_OWNERS], worker_stack=charges[2*MM_OWNERS+1];
    unsigned worker_heap=charges[2*MM_OWNERS+2], created_resident=charges[2*MM_OWNERS+3];
    unsigned touched_resident=charges[2*MM_OWNERS+4];
    const uint32_t quota=touched[MM_VM_POOL], fixed=DOSVM_RESIDENT_CAP/PAGE_SIZE-quota; /* 24 MiB pool */
    CHECK(cache==4096 && created[MM_VM_PRIVATE]>=cache && !created[MM_VM_POOL] && quota==6144);
    const unsigned sizes[]={128,256,512};
    for (unsigned i=0;i<ARRAY_SIZE(sizes);i++) {
        fresh_kernel(sizes[i],15);
        uint32_t heap=pmm_alloc_contig(1024), tables=pmm_alloc_contig(384), syscache=pmm_alloc_contig(2048);
        CHECK(heap && tables && syscache);
        mm_claim(heap,1024,MM_HEAP); mm_claim(tables,384,MM_TABLE); mm_claim(syscache,2048,MM_CACHE_DATA);
        CHECK(ledger_reconcile()); ledger_record("boot-end");
        uint32_t boot=pmm_free_count();
        uint32_t desktop=pmm_alloc_contig(768); CHECK(desktop);
        mm_claim(desktop,768,MM_USER_SHARED); ledger_promise(MM_USER_SHARED,768,768);
        CHECK(ledger_reconcile()); ledger_record("settled-desktop");
        uint32_t settled=pmm_free_count();
        CHECK(vm_admit_margin(settled,0,0)); /* the first default VM is admitted */
        /* Created: fixed promise, pool commitment, creation charges. */
        ledger_promise(MM_VM_PRIVATE,(int32_t)fixed,(int32_t)fixed);
        ledger_promise(MM_VM_POOL,(int32_t)quota,0);
        CHECK(ledger_commit(MM_VM_POOL,quota));
        uint32_t backing[MM_OWNERS]={0}, pool=0;
        for (unsigned o=0;o<MM_OWNERS;o++) if (created[o]) {
            backing[o]=pmm_alloc_contig(created[o]); CHECK(backing[o]);
            mm_claim(backing[o],created[o],o);
        }
        uint32_t stack=pmm_alloc_contig(worker_stack), task=pmm_alloc_contig(worker_heap);
        CHECK(stack && task); mm_claim(stack,worker_stack,MM_STACK); mm_claim(task,worker_heap,MM_HEAP);
        CHECK(ledger_reconcile()); ledger_record("maximum-vm-created");
        struct mm_ledger v=snapshot();
        CHECK(resident(MM_VM_PRIVATE)==created[MM_VM_PRIVATE] && !resident(MM_VM_POOL) && !v.pressure);
        CHECK(v.commit_ram[MM_VM_POOL]==quota && v.commit_ram[MM_VM_PRIVATE]==fixed);
        uint32_t created_free=pmm_free_count();
        int64_t created_headroom=ledger_commit_headroom();
        uint64_t created_margin=vm_admit_margin(created_free,DOSVM_RESIDENT_CAP,created_resident);
        /* Touched: every pool page resident. */
        pool=pmm_alloc_contig(touched[MM_VM_POOL]); CHECK(pool);
        mm_claim(pool,touched[MM_VM_POOL],MM_VM_POOL);
        CHECK(ledger_reconcile()); ledger_record("maximum-vm-touched");
        v=snapshot();
        CHECK(resident(MM_VM_POOL)==quota && !v.pressure && !v.commit_pagefile);
        uint32_t touched_free=pmm_free_count();
        int64_t touched_headroom=ledger_commit_headroom();
        uint64_t touched_margin=vm_admit_margin(touched_free,DOSVM_RESIDENT_CAP,touched_resident);
        /* Same RAM formula at both states: unmaterialized pool commitment is
         * outstanding, so demand-zero never admits more VMs on RAM alone. */
        CHECK(created_margin==touched_margin);
        printf("f3-46 admission model=%u boot_free=%u settled_free=%u created_free=%u touched_free=%u "
               "created_resident=%u touched_resident=%u commit_headroom=%lld/%lld second_vm_margin=%llu/%llu "
               "second_vm=%u/%u fixed_promise=%u pool_commit=%u cache_pages=%u reconciled=1\n",
            sizes[i],boot*PAGE_SIZE,settled*PAGE_SIZE,created_free*PAGE_SIZE,touched_free*PAGE_SIZE,
            created_resident,touched_resident,(long long)created_headroom,(long long)touched_headroom,
            (unsigned long long)created_margin,(unsigned long long)touched_margin,
            created_margin>0,touched_margin>0,fixed,quota,cache);
        mm_release(pool,touched[MM_VM_POOL],MM_VM_POOL); release_run(pool,touched[MM_VM_POOL]);
        for (unsigned o=0;o<MM_OWNERS;o++) if (backing[o]) {
            mm_release(backing[o],created[o],o); release_run(backing[o],created[o]);
        }
        mm_release(stack,worker_stack,MM_STACK); release_run(stack,worker_stack);
        mm_release(task,worker_heap,MM_HEAP); release_run(task,worker_heap);
        ledger_promise(MM_VM_POOL,-(int32_t)quota,-(int32_t)quota);
        ledger_promise(MM_VM_PRIVATE,-(int32_t)fixed,-(int32_t)fixed);
        v=snapshot();
        CHECK(pmm_free_count()==settled && !resident(MM_VM_PRIVATE) && !resident(MM_VM_POOL));
        CHECK(!v.commit_ram[MM_VM_POOL] && !v.commit_ram[MM_VM_PRIVATE] && !v.reserved_va[MM_VM_POOL]);
        ledger_record("vm-teardown");
        mm_release(desktop,768,MM_USER_SHARED); release_run(desktop,768);
        ledger_promise(MM_USER_SHARED,-768,-768);
        mm_release(heap,1024,MM_HEAP); release_run(heap,1024);
        mm_release(tables,384,MM_TABLE); release_run(tables,384);
        mm_release(syscache,2048,MM_CACHE_DATA); release_run(syscache,2048);
    }
}
static struct uaddr *racing_space;
static void unmap_during_fault(void)
{
    memory_service=0;
    CHECK(ua_munmap(racing_space,CIUKI_MMAP_BASE,PAGE_SIZE)==-EBUSY);
}
static void exit_during_fault(void)
{ memory_service=0; racing_space->closing=true; }
static struct memory_object *waiting_object;
static struct ua_extent *waiting_area;
static void complete_other_fault(void)
{
    memory_sleep=0;
    struct ua_slot *s=object_slot(waiting_object,0);
    CHECK(s->waiters==1 && waiting_object->busy);
    slot_link(waiting_object,0,frame_new(false,waiting_area->space->as.pd_phys));
    waiting_object->busy=false;
}
static void cancel_waiting_fault(void)
{ memory_sleep=0; waiting_object->busy=false; waiting_area->generation++; }
static struct ua_slot *retiring_slot;
static uint32_t retiring_phys;
static bool retirement_observed;
static void observe_zero_retirement(void)
{
    if (retiring_slot->frame->phys==retiring_phys) return;
    memory_service=0; retirement_observed=true;
    CHECK(!pmm_page_free(retiring_phys/PAGE_SIZE) && pmm_descriptor(retiring_phys)->refs);
}
static struct uaddr *alias_target;
static bool alias_pending_observed;
static void observe_alias_publication(void)
{
    memory_service=0;
    struct ua_extent *e=ua_find(alias_target,CIUKI_MMAP_BASE);
    if (e) {
        CHECK(e->preparing); alias_pending_observed=true;
        CHECK(ua_fault(alias_target,e->base,4)==-EFAULT);
        CHECK(ua_munmap(alias_target,e->base,PAGE_SIZE)==-EBUSY);
    }
    memory_service=observe_alias_publication;
}
static uint32_t memory_word(struct uaddr *u,uint32_t at)
{ uint32_t value=~0u; CHECK(!ua_read(u,&value,at,4)); return value; }
static void memory_store(struct uaddr *u,uint32_t at,uint32_t value)
{ CHECK(!ua_write(u,at,&value,4)); }
static void memory_objects(void)
{
    fresh(64);
    struct task task={0}; g_current=&task;
    uint32_t baseline=pmm_free_count();
    struct mm_ledger initial=snapshot();
    uint32_t kernel_pages=pmm_zone_free(ZONE_NORMAL), kernel=pmm_alloc_contig(kernel_pages);
    CHECK(kernel); ledger_promise(MM_KERNEL,0,kernel_pages);
    CHECK(ledger_commit(MM_USER_PRIVATE,1)); /* resident commitments are not charged twice */
    ledger_promise(MM_USER_PRIVATE,0,-1); ledger_promise(MM_KERNEL,0,-(int32_t)kernel_pages);
    release_run(kernel,kernel_pages); CHECK(pmm_free_count()==baseline);
    for (unsigned cycle=0;cycle<100;cycle++) {
        struct uaddr u,v;
        CHECK(!ua_init(&u,&u) && !ua_init(&v,&v));
        uint32_t after_directories=pmm_free_count();
        CHECK(!ua_map_at(&u,CIUKI_MMAP_BASE,8*PAGE_SIZE,3,3,UA_ANON,0));
        CHECK(!u.backing && !u.as.tables && pmm_free_count()==after_directories);
        CHECK(snapshot().commit_ram[MM_USER_PRIVATE]==8 && !snapshot().commit_pagefile);
        CHECK(!ua_fault(&u,CIUKI_MMAP_BASE,4));
        CHECK(memory_word(&u,CIUKI_MMAP_BASE)==0);
        uint32_t zero=as_lookup(&u.as,CIUKI_MMAP_BASE,0);
        CHECK(zero && pmm_descriptor(zero)->backing_kind==MM_BACKING_ZERO);
        CHECK(!ua_fault(&u,CIUKI_MMAP_BASE+PAGE_SIZE,4));
        CHECK(as_lookup(&u.as,CIUKI_MMAP_BASE+PAGE_SIZE,0)==zero);
        CHECK(pmm_descriptor(zero)->refs==2);
        CHECK(!ua_fault(&u,CIUKI_MMAP_BASE,7));
        uint32_t own=as_lookup(&u.as,CIUKI_MMAP_BASE,0), stable=pmm_free_count();
        CHECK(own!=zero && !memory_word(&u,CIUKI_MMAP_BASE));
        memory_store(&u,CIUKI_MMAP_BASE,0x12345678);
        memory_store(&u,CIUKI_MMAP_BASE,0x87654321);
        CHECK(pmm_free_count()==stable && pmm_descriptor(own)->dirty_epoch);
        int32_t copy=ua_alias(&v,&u,CIUKI_MMAP_BASE,8*PAGE_SIZE,3,true);
        CHECK(copy>0 && pmm_descriptor(own)->refs==2 && snapshot().commit_ram[MM_USER_PRIVATE]==16);
        CHECK(memory_word(&v,copy)==0x87654321);
        CHECK(!ua_fault(&v,copy,7)); memory_store(&v,copy,42);
        CHECK(*(uint32_t *)P2V(own)==0x87654321 && memory_word(&v,copy)==42);
        CHECK(!ua_fault(&u,CIUKI_MMAP_BASE,7)); /* sole surviving COW reference */
        int32_t shared=ua_alias(&v,&u,CIUKI_MMAP_BASE,8*PAGE_SIZE,3,false);
        CHECK(shared>0 && shared!=copy && snapshot().commit_ram[MM_USER_PRIVATE]==16);
        CHECK(!memory_word(&v,shared+2*PAGE_SIZE));
        CHECK(!memory_word(&u,CIUKI_MMAP_BASE+2*PAGE_SIZE));
        memory_store(&v,shared+2*PAGE_SIZE,99);
        CHECK(memory_word(&u,CIUKI_MMAP_BASE+2*PAGE_SIZE)==99);
        CHECK(!ua_mprotect(&v,copy,PAGE_SIZE,PROT_READ));
        CHECK(ua_fault(&v,copy,7)==-EFAULT);
        CHECK(!ua_mprotect(&v,copy,PAGE_SIZE,PROT_NONE));
        CHECK(ua_fault(&v,copy,4)==-EFAULT);
        CHECK(ua_fault(&v,CIUKI_MMAP_BASE+100*PAGE_SIZE,4)==-EFAULT);
        CHECK(ua_fault(&u,CIUKI_MMAP_BASE,0)==-EFAULT && ua_fault(&u,CIUKI_MMAP_BASE,12)==-EFAULT);
        CHECK(!ua_map_at(&v,CIUKI_MAIN_STACK_BASE,PAGE_SIZE,0,0,UA_GUARD,1));
        CHECK(ua_fault(&v,CIUKI_MAIN_STACK_BASE,4)==-EFAULT);
        ua_destroy(&u); ua_destroy(&v);
        struct mm_ledger after=snapshot();
        CHECK(pmm_free_count()==baseline && !zero_frame && ledger_reconcile());
        CHECK(!memcmp(after.commit_ram,initial.commit_ram,sizeof(after.commit_ram)));
        CHECK(!memcmp(after.reserved_va,initial.reserved_va,sizeof(after.reserved_va)));
        CHECK(!after.mappings && !after.shared_mappings && !after.mismatches && !after.pin_tokens);
    }
    /* Allocation failure at every area/object/block/frame/PT stage. Each
     * failed fault keeps its commitment, publishes no uninitialized frame. */
    for (int point=0;point<9;point++) {
        struct uaddr u; CHECK(!ua_init(&u,&u));
        memory_failure=point;
        int err=ua_map_at(&u,CIUKI_MMAP_BASE,65*PAGE_SIZE,3,3,UA_ANON,0);
        memory_failure=-1;
        CHECK(!err || err==-ENOMEM);
        if (err) CHECK(!u.head && !u.backing && !u.as.tables);
        ua_destroy(&u); CHECK(pmm_free_count()==baseline && ledger_reconcile());
    }
    for (int point=0;point<4;point++) for (unsigned write=0;write<2;write++) {
        struct uaddr u; CHECK(!ua_init(&u,&u));
        CHECK(!ua_map_at(&u,CIUKI_MMAP_BASE,PAGE_SIZE,3,3,UA_ANON,0));
        uint32_t before=pmm_free_count();
        memory_failure=point; int err=ua_fault(&u,CIUKI_MMAP_BASE,4|(write?2:0)); memory_failure=-1;
        if (point<3) CHECK(err==-ENOMEM && !u.backing && !u.as.tables && pmm_free_count()==before);
        else CHECK(!err);
        CHECK(snapshot().commit_ram[MM_USER_PRIVATE]==1);
        ua_destroy(&u); CHECK(pmm_free_count()==baseline && ledger_reconcile());
    }
    struct uaddr u,v; CHECK(!ua_init(&u,&u) && !ua_init(&v,&v));
    CHECK(!ua_map_at(&u,CIUKI_MMAP_BASE,32*PAGE_SIZE,3,3,UA_ANON,0));
    for (unsigned i=0;i<32;i++) memory_store(&u,CIUKI_MMAP_BASE+i*PAGE_SIZE,i+1);
    alias_target=&v; alias_pending_observed=false; memory_service=observe_alias_publication;
    CHECK(ua_alias(&v,&u,CIUKI_MMAP_BASE,32*PAGE_SIZE,3,true)>0);
    memory_service=0; CHECK(alias_pending_observed);
    for (unsigned i=0;i<32;i++) {
        uint32_t entry=0; CHECK(as_lookup(&u.as,CIUKI_MMAP_BASE+i*PAGE_SIZE,&entry) && !(entry&PTE_W));
    }
    ua_destroy(&u); ua_destroy(&v); CHECK(pmm_free_count()==baseline && ledger_reconcile());
    CHECK(!ua_init(&u,&u));
    CHECK(!ua_map_at(&u,CIUKI_MMAP_BASE,PAGE_SIZE,3,3,UA_ANON,0));
    struct memory_object *placeholder=ua_find(&u,CIUKI_MMAP_BASE)->backing;
    const enum mm_backing_kind unsupported[]={MM_BACKING_FILE,MM_BACKING_SWAP,MM_BACKING_DEVICE};
    uint32_t untouched=pmm_free_count();
    for (unsigned i=0;i<3;i++) {
        placeholder->kind=unsupported[i];
        CHECK(ua_fault(&u,CIUKI_MMAP_BASE,4)==-EFAULT && !u.backing && pmm_free_count()==untouched);
    }
    ua_destroy(&u); CHECK(pmm_free_count()==baseline && ledger_reconcile());
    CHECK(!ua_init(&u,&u)); racing_space=&u;
    CHECK(!ua_map_at(&u,CIUKI_MMAP_BASE,2*PAGE_SIZE,3,3,UA_ANON,0));
    memory_service=unmap_during_fault;
    CHECK(!ua_fault(&u,CIUKI_MMAP_BASE,6) && !memory_service);
    memory_service=exit_during_fault;
    CHECK(ua_fault(&u,CIUKI_MMAP_BASE+PAGE_SIZE,6)==-EFAULT && !memory_service);
    ua_destroy(&u); CHECK(pmm_free_count()==baseline && ledger_reconcile());
    CHECK(!ua_init(&u,&u));
    CHECK(!ua_map_at(&u,CIUKI_MMAP_BASE,PAGE_SIZE,3,3,UA_ANON,0));
    CHECK(!ua_fault(&u,CIUKI_MMAP_BASE,4));
    for (unsigned alias=0;alias<128;alias++) {
        int32_t at=ua_alias(&u,&u,CIUKI_MMAP_BASE,PAGE_SIZE,3,false);
        CHECK(at>0 && !ua_fault(&u,at,4));
    }
    retiring_slot=area_slot(ua_find(&u,CIUKI_MMAP_BASE),CIUKI_MMAP_BASE);
    retiring_phys=retiring_slot->frame->phys; retirement_observed=false;
    memory_service=observe_zero_retirement; memory_store(&u,CIUKI_MMAP_BASE,123);
    CHECK(retirement_observed && !memory_service && pmm_page_free(retiring_phys/PAGE_SIZE));
    for (struct ua_extent *e=u.head;e;e=e->next) CHECK(memory_word(&u,e->base)==123);
    ua_destroy(&u); CHECK(pmm_free_count()==baseline && ledger_reconcile());
    CHECK(!ua_init(&u,&u));
    CHECK(!ua_map_at(&u,CIUKI_MMAP_BASE,PAGE_SIZE,3,3,UA_ANON,0));
    waiting_area=ua_find(&u,CIUKI_MMAP_BASE); waiting_object=waiting_area->backing;
    waiting_object->busy=true; memory_sleep=complete_other_fault;
    CHECK(!ua_fault(&u,CIUKI_MMAP_BASE,6) && !memory_sleep && waiting_object->resident==1);
    waiting_object->busy=true; memory_sleep=cancel_waiting_fault;
    CHECK(ua_fault(&u,CIUKI_MMAP_BASE,6)==-EFAULT && !memory_sleep);
    ua_destroy(&u); CHECK(pmm_free_count()==baseline && ledger_reconcile());
    /* Pin preparation is rejected before a pager can enter a held lock. A
     * prepared range subsequently copies under that lock without allocating. */
    CHECK(!ua_init(&u,&u)); CHECK(!ua_map_at(&u,CIUKI_MMAP_BASE,PAGE_SIZE,3,3,UA_ANON,0));
    struct ua_pin pin; task.preempt_holds=1;
    CHECK(ua_pin(&u,&pin,CIUKI_MMAP_BASE,4,false)==-EBUSY && !u.backing && !u.pins);
    task.preempt_holds=0; CHECK(!ua_pin(&u,&pin,CIUKI_MMAP_BASE,4,false));
    uint32_t phys=as_lookup(&u.as,CIUKI_MMAP_BASE,0), before=pmm_free_count();
    CHECK(pmm_descriptor(phys)->pins==1);
    task.preempt_holds=1; CHECK(!memory_word(&u,CIUKI_MMAP_BASE) && pmm_free_count()==before);
    task.preempt_holds=0; ua_unpin(&u,&pin); CHECK(!pmm_descriptor(phys)->pins);
    ua_destroy(&u); CHECK(pmm_free_count()==baseline && ledger_reconcile());
    CHECK(!ua_init(&u,&u)); racing_space=&u;
    CHECK(!ua_map_at(&u,CIUKI_MMAP_BASE,32*PAGE_SIZE,3,3,UA_ANON,0));
    CHECK(!ua_pin(&u,&pin,CIUKI_MMAP_BASE,32*PAGE_SIZE,true));
    memory_service=unmap_during_fault; ua_unpin(&u,&pin); CHECK(!memory_service && !u.pins);
    ua_destroy(&u); CHECK(pmm_free_count()==baseline && ledger_reconcile()); g_current=0;
    puts("PASS M2: 100 cycles, deferred zero/COW/shared aliases, permissions/guards, all allocation stages, unmap/exit interleavings, coalescing/wake generation, pin-before-lock, ledger reconciliation");
}

int main(int argc, char **argv)
{
    ram=calloc(1,(argc>1 ? 512u : 64u)<<20); CHECK(ram);
    if (argc>1) {
        CHECK(!strcmp(argv[1],"--dosvm-profile")); emit_profiles=true;
        dosvm_profiles(argc,argv); free(ram); return 0;
    }
    profiles(); memory_objects(); aliases_and_rollback(); pins(); scattered_pool(); reporting_and_context(); pinned_context();
    struct mm_ledger maximum=snapshot();
    maximum.mappings=maximum.shared_mappings=maximum.pin_tokens=maximum.pinned_pages=UINT32_MAX;
    maximum.retired_pages=maximum.mismatches=maximum.double_unpins=maximum.bad_context=UINT32_MAX;
    record_snapshot("maximum-vm-created",&maximum,UINT64_MAX);
    printf("PASS resource ledger checks=%u static_data=%zu bytes invlpg=%u cr3_reload=%u strict=%u\n",checks,sizeof(ledger),invalidations,reloads,
#ifdef CIUKI_LEDGER_STRICT
        1u
#else
        0u
#endif
    );
    free(ram); return 0;
}
