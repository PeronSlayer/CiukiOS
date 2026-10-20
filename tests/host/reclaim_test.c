/* M3 production PMM/ledger/VMM/uaddr/scanner, controlled UP interleavings.
 * The cache here is a frame-release model; the real C4 hook is also exercised
 * by host_fs_tests. No host thread executes privileged code or page-out I/O.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main ledger_fixture_main
#include "ledger_test.c"
#undef main
#undef CLOCKS_PER_SEC
#include <time.h>
#include <ciuki/reclaim.h>
static uint64_t model_cycles;
static uint32_t clock_charge=1000;
uint64_t g_tsc_per_ms=1000000;
uint64_t cpu_path_clock(void) { model_cycles+=clock_charge; return model_cycles; }
uint64_t ktime_cycles(void) { return cpu_path_clock(); }
static unsigned wakes;
void kwait_wake_irq(struct kwait *q,bool io) { (void)q; CHECK(!io); wakes++; }
struct block_cache { uint32_t phys[1536], count, cursor; bool speculative; unsigned first_clean; };
void cache_reclaim_step(struct block_cache *c,bool speculative,struct reclaim_result *r)
{
    if (c->speculative!=speculative) { c->speculative=speculative; c->cursor=0; }
    *r=(struct reclaim_result){.scanned=1};
    if (c->cursor==c->count) { r->done=true; c->cursor=0; return; }
    unsigned i=c->cursor++;
    if (c->phys[i] && (!speculative || i<256)) {
        if (!speculative && !c->first_clean) c->first_clean=i;
        if (!speculative) for (unsigned j=0;j<256;j++) CHECK(!c->phys[j]);
        mm_free(c->phys[i],MM_CACHE_DATA); c->phys[i]=0; r->reclaimed=1;
    }
}
#include "../../src/kernel/core/reclaim.c"
static uint64_t host_cpu_us(void)
{ clock_t t=clock(); CHECK(t!=(clock_t)-1); return (uint64_t)t*1000000/CLOCKS_PER_SEC; }
static uint32_t max_pages, max_model_us;
static uint64_t max_cpu_us;
static void slice_checked(void)
{
    uint32_t before=snapshot().reclaim_scanned;
    uint64_t cycles=model_cycles, cpu=host_cpu_us();
    reclaim_slice();
    uint32_t pages=snapshot().reclaim_scanned-before;
    uint64_t cpu_us=host_cpu_us()-cpu;
    uint32_t us=(uint32_t)((model_cycles-cycles)/1000);
    CHECK(pages<=64 && us<=1000);
    if (pages>max_pages) max_pages=pages;
    if (us>max_model_us) max_model_us=us;
    if (cpu_us>max_cpu_us) max_cpu_us=cpu_us;
}
static uint32_t pressure_hold(void)
{
    struct mm_ledger v=snapshot();
    uint32_t pages=v.zone[ZONE_NORMAL].free-v.low+1, p=pmm_alloc_contig(pages);
    CHECK(p && ledger_pressure()); return p;
}
static void release_pressure(uint32_t p)
{
    uint32_t count=0;
    while (p/PAGE_SIZE+count<descriptor_count && !pmm_page_free(p/PAGE_SIZE+count) &&
           (pmm_descriptor(p+count*PAGE_SIZE)->reserved&MM_PAGE_OWNER_MASK)==MM_KERNEL) count++;
    release_run(p,count);
}
static void fit_after_cache(void)
{
    fresh(128); uint32_t baseline=pmm_free_count();
    struct uaddr interactive; CHECK(!ua_init(&interactive,&interactive));
    CHECK(!ua_map_at(&interactive,CIUKI_MMAP_BASE,16*PAGE_SIZE,3,3,UA_ANON,0));
    for (unsigned i=0;i<16;i++) memory_store(&interactive,CIUKI_MMAP_BASE+i*PAGE_SIZE,0x1234+i);
    struct block_cache cache={.count=1536};
    for (unsigned i=0;i<cache.count;i++) { cache.phys[i]=pmm_alloc(); CHECK(cache.phys[i]); mm_claim(cache.phys[i],1,MM_CACHE_DATA); }
    reclaim_cache_register(&cache);
    uint32_t held=pressure_hold(), rounds=0;
    while (ledger_pressure()) { CHECK(rounds++<1000); slice_checked(); }
    struct mm_ledger v=snapshot(); struct reclaim_io io;
    CHECK(v.zone[2].free>v.high && v.reclaim_reclaimed==v.high-v.low+2);
    CHECK(cache.first_clean && !reclaim_writeback_take(&io));
    for (unsigned i=0;i<16;i++) CHECK(memory_word(&interactive,CIUKI_MMAP_BASE+i*PAGE_SIZE)==0x1234+i);
    printf("TRACE M3 fits low=%u high=%u slices=%u scanned=%u cache_reclaimed=%u anon_queued=0 interactive=16/16\n",
        v.low,v.high,rounds,v.reclaim_scanned,v.reclaim_reclaimed);
    reclaim_poll(1000); CHECK(pending.episodes==0 && last_report==1000);
    reclaim_cache_unregister(&cache);
    release_pressure(held);
    for (unsigned i=0;i<cache.count;i++) if (cache.phys[i]) mm_free(cache.phys[i],MM_CACHE_DATA);
    ua_destroy(&interactive); CHECK(!resident_head && pmm_free_count()==baseline && ledger_reconcile());
}
struct test_page {
    struct reclaim_page page;
    struct uaddr *space;
    uint32_t va, refs, revoked;
    bool exiting, discarded, partial, failed;
};
static bool provider_valid(struct reclaim_page *p)
{
    struct test_page *t=(void *)p;
    /* Synthetic backing is test-only. Freeze both real PTE aliases before
     * the queue owns this frame; f0-49 must implement the actual pager. */
    for (unsigned i=0;i<2;i++) {
        uint32_t va=t->va+i*PAGE_SIZE, *entry=pte(t->space,va,false);
        set_pte(t->space,va,entry,*entry&~PTE_W);
    }
    return true;
}
static void provider_retain(struct reclaim_page *p) { ((struct test_page *)p)->refs++; }
static bool provider_release(struct reclaim_page *p)
{
    struct test_page *t=(void *)p; CHECK(t->refs>1); t->refs--;
    if (t->exiting || t->discarded) { CHECK(t->refs==1); reclaim_untrack(p); mm_free(p->phys,p->owner); t->refs=0; return true; }
    return false;
}
static bool provider_finish(struct reclaim_page *p,const struct reclaim_io *io,bool success)
{
    struct test_page *t=(void *)p; CHECK(io && io->ticket);
    CHECK(p->queued && pmm_descriptor(p->phys)->pins && t->refs==2);
    if (!success && !t->exiting) { t->failed=true; return true; }
    unsigned limit=t->partial?t->revoked+1:2;
    while (t->revoked<limit) {
        uint32_t va=t->va+t->revoked*PAGE_SIZE, *entry=pte(t->space,va,false);
        set_pte(t->space,va,entry,0); t->revoked++;
    }
    if (t->revoked!=2) return false;
    t->discarded=true; return true;
}
static const struct reclaim_backing provider={provider_valid,provider_retain,provider_release,provider_finish,0,0};
static void owner_traces(void)
{
    fresh(128); uint32_t baseline=pmm_free_count();
    struct uaddr u; CHECK(!ua_init(&u,&u)); cr3=u.as.pd_phys;
    struct aspace dos; CHECK(!as_create(&dos)); dos.owner=MM_VM_POOL;
    uint32_t dos_phys[8];
    for (unsigned i=0;i<8;i++) {
        dos_phys[i]=pmm_alloc(); CHECK(dos_phys[i]); mm_claim(dos_phys[i],1,MM_VM_POOL);
        CHECK(!as_map(&dos,0x100000+i*PAGE_SIZE,dos_phys[i],PTE_U|PTE_W));
    }
    uint32_t *dos_entries=table_va(table_va(dos.pd_phys)[0]&~4095u)+0x100;
    struct test_page pages[51]={0};
    for (unsigned i=0;i<51;i++) {
        struct test_page *t=&pages[i];
        t->space=&u; t->va=CIUKI_MMAP_BASE+i*2*PAGE_SIZE; t->refs=1;
        t->page=(struct reclaim_page){.phys=pmm_alloc(),.generation=i+1,
            .owner=MM_USER_PRIVATE,.owner_id=i<16?1:2,.backing=&provider};
        CHECK(t->page.phys); mm_claim(t->page.phys,1,MM_USER_PRIVATE); reclaim_track(&t->page);
        for (unsigned a=0;a<2;a++) CHECK(!as_map(&u.as,t->va+a*PAGE_SIZE,t->page.phys,PTE_U|PTE_W));
    }
    pmm_descriptor(pages[50].page.phys)->pins=1;
    pmm_descriptor(pages[49].page.phys)->io_in_flight=1;
    uint32_t held=pressure_hold(), queued=0;
    /* Complete three global alias/age passes: no hash- or alias-order bias.
     * Mark interactive PTEs accessed on every slice; cold streaming pages
     * remain untouched after their initial second chances. */
    for (unsigned n=0;n<10000 && queued<32;n++) {
        for (unsigned i=0;i<16;i++) *pte(&u,pages[i].va,false)|=PTE_A;
        for (unsigned i=0;i<8;i++) dos_entries[i]|=PTE_A;
        uint32_t before=0; for (unsigned i=16;i<51;i++) before+=pages[i].page.queued;
        slice_checked();
        uint32_t after=0; for (unsigned i=16;i<51;i++) after+=pages[i].page.queued;
        CHECK(after-before<=RECLAIM_OWNER_LIMIT);
        queued=after;
    }
    CHECK(queued==32 && !pages[49].page.queued && !pages[50].page.queued);
    CHECK(snapshot().reclaim_pinned && snapshot().reclaim_inflight);
    for (unsigned i=0;i<16;i++) CHECK(!pages[i].page.queued && (pmm_descriptor(pages[i].page.phys)->reserved&MM_PAGE_HOT));
    for (unsigned i=0;i<8;i++) CHECK(pmm_descriptor(dos_phys[i])->reserved&MM_PAGE_HOT);
    printf("TRACE M3 owners interactive_resident=16 hot=16 evicted=0 streaming_resident=35 cold=35 queued=%u aliases=102 physical=51\n",queued);
    puts("TRACE M3 DOS resident=8 hot=8 evicted=0 pool_policy=unchanged");
    struct reclaim_io io, stale;
    CHECK(reclaim_writeback_take(&io)); stale=io;
    struct test_page *first=&pages[io.generation-1]; first->partial=true; first->exiting=true;
    CHECK(!reclaim_writeback_complete(&io,true));
    CHECK(first->refs==2 && first->revoked==1 && !pmm_page_free(io.phys/PAGE_SIZE));
    CHECK(reclaim_writeback_complete(&io,true) && pmm_page_free(io.phys/PAGE_SIZE));
    CHECK(!reclaim_writeback_complete(&stale,true));
    /* Redirty between selection and completion makes old write completion
     * unable to clear the newer modification or free this resident page. */
    CHECK(reclaim_writeback_take(&io));
    struct test_page *second=&pages[io.generation-1];
    pmm_descriptor(io.phys)->dirty_epoch++;
    CHECK(reclaim_writeback_complete(&io,true));
    CHECK(second->failed && second->refs==1 && pmm_descriptor(io.phys)->dirty_epoch);
    while (reclaim_writeback_take(&io)) CHECK(reclaim_writeback_complete(&io,true));
    release_pressure(held); reclaim_poll(2000);
    pmm_descriptor(pages[50].page.phys)->pins=0;
    pmm_descriptor(pages[49].page.phys)->io_in_flight=0;
    for (unsigned i=0;i<51;i++) if (pages[i].refs) {
        struct test_page *t=&pages[i];
        for (unsigned a=0;a<2;a++) { uint32_t va=t->va+a*PAGE_SIZE,*entry=pte(&u,va,false); set_pte(&u,va,entry,0); }
        reclaim_untrack(&t->page); mm_free(t->page.phys,MM_USER_PRIVATE);
    }
    as_destroy(&dos); as_destroy(&u.as); cr3=0;
    CHECK(!resident_head && !sample_spaces && pmm_free_count()==baseline && ledger_reconcile());
    puts("TRACE M3 queue depth=32 owner_per_slice<=8 stale_completion=rejected partial_alias_exit=retained redirty=preserved double_free=0");
}
static void aliases_and_budget(void)
{
    fresh(128); struct uaddr u; CHECK(!ua_init(&u,&u));
    CHECK(!ua_map_at(&u,CIUKI_MMAP_BASE,PAGE_SIZE,3,3,UA_ANON,0)); memory_store(&u,CIUKI_MMAP_BASE,7);
    uint32_t phys=as_lookup(&u.as,CIUKI_MMAP_BASE,0), *entry=pte(&u,CIUKI_MMAP_BASE,false);
    *entry|=PTE_A|PTE_D; cr3=u.as.pd_phys;
    unsigned before_invalidations=invalidations;
    CHECK((as_sample_page(&u.as,CIUKI_MMAP_BASE)&(PTE_A|PTE_D))==(PTE_A|PTE_D));
    CHECK(invalidations==before_invalidations+1);
    CHECK(!(*entry&(PTE_A|PTE_D)) && as_lookup(&u.as,CIUKI_MMAP_BASE,0)==phys);
    uint32_t held=pressure_hold(); clock_charge=60000;
    uint32_t before=snapshot().reclaim_scanned; slice_checked();
    CHECK(snapshot().reclaim_scanned-before<64);
    clock_charge=1000; g_tsc_per_ms=0; before=snapshot().reclaim_scanned;
    slice_checked(); CHECK(snapshot().reclaim_scanned-before==1); g_tsc_per_ms=1000000;
    cr3=0; ua_destroy(&u); CHECK(!sample_spaces && !resident_head);
    slice_checked(); /* owner exited while the global sample was incomplete */
    release_pressure(held); reclaim_poll(2100);
    CHECK(pending.episodes && last_report==2000); reclaim_poll(3000); CHECK(!pending.episodes);
    CHECK(!sample_spaces && !resident_head && ledger_reconcile());
    puts("TRACE M3 sampled_clear=translation_retained time_guard=stopped_before_64 pressure=low/high_hysteresis reporting<=1/s");
}
/* f3-46: the sampler clears and folds A/D only for replaceable, unpinned
 * frames. A DOS VM's VGA window (VM_PRIVATE) and pinned VBE buffer keep
 * their PTE dirty bits for the model's own harvest. */
static void sampler_owner_policy(void)
{
    fresh(128); uint32_t baseline=pmm_free_count();
    struct aspace as; CHECK(!as_create(&as));
    uint32_t vga=pmm_alloc(), vbe=pmm_alloc(), pool=pmm_alloc(); CHECK(vga && vbe && pool);
    mm_claim(vga,1,MM_VM_PRIVATE); mm_claim(vbe,1,MM_VM_POOL); mm_claim(pool,1,MM_VM_POOL);
    CHECK(!as_map(&as,0xa0000,vga,PTE_U|PTE_W) && !as_map(&as,0x110000,vbe,PTE_U|PTE_W) &&
          !as_map(&as,0x111000,pool,PTE_U|PTE_W));
    pmm_descriptor(vbe)->pins=1;
    uint32_t *pd=P2V(as.pd_phys), *pt=P2V(pd[0]&~4095u);
    pt[0xa0]|=PTE_A|PTE_D; pt[0x110]|=PTE_A|PTE_D; pt[0x111]|=PTE_A|PTE_D;
    uint32_t dirty=pmm_descriptor(pool)->dirty_epoch;
    CHECK(!(as_sample_page(&as,0xa0000)&(PTE_A|PTE_D)) && (pt[0xa0]&(PTE_A|PTE_D))==(PTE_A|PTE_D));
    CHECK(!(as_sample_page(&as,0x110000)&(PTE_A|PTE_D)) && (pt[0x110]&(PTE_A|PTE_D))==(PTE_A|PTE_D));
    CHECK((as_sample_page(&as,0x111000)&(PTE_A|PTE_D))==(PTE_A|PTE_D) && !(pt[0x111]&(PTE_A|PTE_D)));
    sample_space=&as; sample_va=0xa0000;
    for (unsigned i=0;i<0x72;i++) vmm_sample_step(9); /* 0xa0000..0x111000 */
    CHECK(pmm_descriptor(pool)->dirty_epoch==dirty && !(pmm_descriptor(vga)->reserved&MM_PAGE_TOUCHED));
    pt[0x111]|=PTE_D; sample_space=&as; sample_va=0x111000; vmm_sample_step(9);
    CHECK(pmm_descriptor(pool)->dirty_epoch==dirty+1 && !(pt[0x111]&PTE_D));
    pmm_descriptor(vbe)->pins=0;
    pt[0xa0]=pt[0x110]=pt[0x111]=0; as.pages-=3; g_user_mappings-=3; ledger_mapping(-3,0);
    mm_free(vga,MM_VM_PRIVATE); mm_free(vbe,MM_VM_POOL); mm_free(pool,MM_VM_POOL);
    as_destroy(&as);
    CHECK(pmm_free_count()==baseline && ledger_reconcile());
    puts("TRACE M3 sampler: VM_PRIVATE and pinned translations keep A/D, VM_POOL folded once PASS");
}
static struct uaddr *free_observer_space;
static uint32_t free_observer_phys;
static bool free_observed;
static void observe_eager_free(void)
{
    if (pmm_page_free(free_observer_phys/PAGE_SIZE)) {
        CHECK(!as_lookup(&free_observer_space->as,CIUKI_MMAP_BASE,0));
        free_observed=true;
    }
}
static void eager_alias_reuse(void)
{
    fresh(128); uint32_t baseline=pmm_free_count();
    struct uaddr u; CHECK(!ua_init(&u,&u));
    struct ua_extent eager={.base=CIUKI_MMAP_BASE,.end=CIUKI_MMAP_BASE+PAGE_SIZE,
        .prot=3,.maximum=3,.kind=UA_W32,.w32_type=W32_MEM_IMAGE};
    u.head=&eager;
    uint32_t phys=pmm_alloc(); CHECK(phys); mm_claim(phys,1,MM_USER_PRIVATE);
    CHECK(ledger_commit(MM_USER_PRIVATE,1));
    CHECK(!as_map(&u.as,CIUKI_MMAP_BASE,phys,PTE_U|PTE_W));
    *pte(&u,CIUKI_MMAP_BASE,false)|=PTE_OWN; u.backing=1;
    free_observer_space=&u; free_observer_phys=phys; free_observed=false;
    memory_irq_restore=observe_eager_free; cr3=u.as.pd_phys;
    free_pages(&u,CIUKI_MMAP_BASE,CIUKI_MMAP_BASE+PAGE_SIZE,false);
    memory_irq_restore=0; CHECK(free_observed && !u.backing);
    /* Device/firmware aliases do not create resident RAM in the ledger. */
    CHECK(!as_map(&u.as,0xa0000,0xa0000,PTE_U));
    *pte(&u,0xa0000,false)|=PTE_A|PTE_D;
    sample_space=&u.as; sample_va=0xa0000; CHECK(!vmm_sample_step(123));
    CHECK(!(pmm_descriptor(0xa0000)->reserved&MM_PAGE_TOUCHED));
    set_pte(&u,0xa0000,pte(&u,0xa0000,false),0);
    cr3=0; u.head=0; as_destroy(&u.as);
    CHECK(pmm_free_count()==baseline && ledger_reconcile());
    puts("TRACE M3 eager translation invalidated before first PMM-free publication: PASS");
}
int main(void)
{
    ram=calloc(1,64u<<20); CHECK(ram);
    fit_after_cache(); owner_traces(); aliases_and_budget(); eager_alias_reuse(); sampler_owner_policy(); CHECK(!wakes);
    memory_objects(); CHECK(snapshot().reclaim_stalls>=2 && !resident_head && !sample_spaces);
    printf("PASS M3 max_scans=%u modeled_max_us=%u host_ASan_cpu_max_us=%llu\n",max_pages,max_model_us,(unsigned long long)max_cpu_us);
    free(ram); return 0;
}
