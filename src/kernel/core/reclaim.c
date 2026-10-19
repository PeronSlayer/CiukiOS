/* M3 global second chance. Constants here are CiukiOS policy, not Win98
 * constants. Sampling and selection never authorize reuse on their own.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/reclaim.h>
#include <ciuki/cpustat.h>
#include <ciuki/timing.h>
#include <ciuki/sync.h>
#include <ciuki/pager.h>

extern void ledger_reclaim_account(const struct reclaim_result *, unsigned, unsigned, unsigned);
extern void ledger_reclaim_owner_delta(unsigned, int32_t);
#ifndef CIUKI_VMM_SAMPLER
extern void vmm_sample_reset(void) __attribute__((weak));
#endif
static struct block_cache *reclaim_cache;
static struct reclaim_page *resident_head, *resident_cursor, *prewrite_cursor;
static struct kwait pressure_wait;
static bool worker_online, active;
static uint32_t work_active;
static bool cache_in_use;
static uint32_t epoch=1, age_pfn, phase, next_ticket;
static uint64_t last_report;
static struct mm_ledger episode_start;
static struct {
    uint32_t episodes, scanned, reclaimed, dirty, inflight, pinned, stalls, faults, max_us;
    int32_t changes[MM_OWNERS];
} pending;
static struct {
    struct reclaim_page *page;
    struct reclaim_io io;
    uint32_t dirty_epoch;
    bool taken, completing;
} modified[RECLAIM_QUEUE_SIZE];
static unsigned prewrite_queued;
static void end_episode(const struct mm_ledger *);

static uint64_t reclaim_clock(void) { return cpu_path_clock?cpu_path_clock():ktime_cycles(); }
static uint32_t reclaim_us(uint64_t start)
{
    uint64_t d=reclaim_clock()-start;
    return g_tsc_per_ms?(uint32_t)(d/g_tsc_per_ms*1000+(d%g_tsc_per_ms)*1000/g_tsc_per_ms):0;
}
void reclaim_wake(void)
{
    struct mm_ledger v; ledger_snapshot(&v);
    if (!active && v.pressure) {
        episode_start=v; active=true; phase=age_pfn=0;
        ledger_reclaim_account(0,1,0,0);
    } else if (active && !v.pressure && !work_active) end_episode(&v);
    /* M1 invokes this under PMM exclusion: only capture a deferred wake. */
    if (worker_online) kwait_wake_irq(&pressure_wait,false);
}
void reclaim_cache_register(struct block_cache *c)
{ uint32_t f=irq_save(); reclaim_cache=c; irq_restore(f); }
void reclaim_cache_unregister(struct block_cache *c)
{
    uint32_t f=irq_save(); bool ours=reclaim_cache==c;
    if (ours) reclaim_cache=0;
    irq_restore(f);
    /* Cache destruction is quiescent task context. Drain a preflight that
     * might already be waiting for its lock before freeing descriptor pages. */
    while (ours) {
        f=irq_save(); bool busy=cache_in_use; irq_restore(f);
        if (!busy) break;
        task_sleep_ms(1);
    }
}
void reclaim_track(struct reclaim_page *p)
{
    uint32_t f=irq_save();
    if (!p->linked) {
        p->prev=0; p->next=resident_head;
        if (resident_head) resident_head->prev=p;
        resident_head=p; p->linked=true; p->chances=2;
    }
    irq_restore(f);
}
void reclaim_untrack(struct reclaim_page *p)
{
    uint32_t f=irq_save();
    /* A queued page has a provider reference until completion, including
     * during exit. A premature untrack is refused without freeing its slot. */
    if (p->linked && !p->queued) {
        if (resident_cursor==p) resident_cursor=p->next;
        if (prewrite_cursor==p) prewrite_cursor=p->next;
        if (p->prev) p->prev->next=p->next; else resident_head=p->next;
        if (p->next) p->next->prev=p->prev;
        p->linked=false; p->prev=p->next=0;
    }
    irq_restore(f);
}
void reclaim_fault(bool stalled)
{
    uint32_t f=irq_save();
    ledger_reclaim_account(0,0,!stalled,stalled);
    irq_restore(f);
}
static uint32_t total_owner(const struct mm_ledger *v,unsigned owner)
{ return v->zone[0].resident[owner]+v->zone[1].resident[owner]+v->zone[2].resident[owner]; }
static void capture_episode(const struct mm_ledger *v)
{
    pending.scanned+=v->reclaim_scanned-episode_start.reclaim_scanned;
    pending.reclaimed+=v->reclaim_reclaimed-episode_start.reclaim_reclaimed;
    pending.dirty+=v->reclaim_dirty-episode_start.reclaim_dirty;
    pending.inflight+=v->reclaim_inflight-episode_start.reclaim_inflight;
    pending.pinned+=v->reclaim_pinned-episode_start.reclaim_pinned;
    pending.stalls+=v->reclaim_stalls-episode_start.reclaim_stalls;
    pending.faults+=v->reclaim_faults-episode_start.reclaim_faults;
    for (unsigned o=0;o<MM_OWNERS;o++) {
        int32_t delta=(int32_t)total_owner(v,o)-(int32_t)total_owner(&episode_start,o);
        pending.changes[o]+=delta; ledger_reclaim_owner_delta(o,delta);
    }
    episode_start=*v;
}
static void end_episode(const struct mm_ledger *v)
{
    capture_episode(v); pending.episodes++;
    active=false; phase=age_pfn=0;
}
void reclaim_poll(uint64_t now)
{
    uint32_t f=irq_save();
    struct mm_ledger v; ledger_snapshot(&v);
    if (active && !v.pressure && !work_active) end_episode(&v);
    if ((!pending.episodes && !active) || (last_report && now-last_report<1000)) { irq_restore(f); return; }
    if (active) capture_episode(&v);
    /* Exact per-category resident snapshots remain in the 30 s ledger.
     * The single protected episode summary coalesces rapid pressure bursts. */
    uint32_t episodes=pending.episodes, scanned=pending.scanned, reclaimed=pending.reclaimed;
    uint32_t dirty=pending.dirty, inflight=pending.inflight, pinned=pending.pinned;
    uint32_t stalls=pending.stalls, faults=pending.faults, max_us=pending.max_us, changed=0;
    for (unsigned o=0;o<MM_OWNERS;o++) changed+=pending.changes[o]!=0;
    int32_t du=pending.changes[MM_USER_PRIVATE]+pending.changes[MM_USER_SHARED];
    int32_t dc=pending.changes[MM_CACHE_DATA]+pending.changes[MM_CACHE_META];
    int32_t dv=pending.changes[MM_VM_PRIVATE]+pending.changes[MM_VM_POOL];
    bool ongoing=active;
    memset(&pending,0,sizeof(pending)); last_report=now?now:1;
    irq_restore(f);
    klog("[mem] reclaim episodes=%u active=%u scanned=%u reclaimed=%u dirty=%u inflight=%u pinned=%u stalls=%u faults=%u max_us=%u owners=%u du=%d dc=%d dv=%d",
         episodes,ongoing,scanned,reclaimed,dirty,inflight,pinned,stalls,faults,max_us,changed,du,dc,dv);
}
static bool queue_page(struct reclaim_page *p,struct mm_page_descriptor *d,bool prewrite)
{
    if (!p->backing || !p->backing->valid || !p->backing->retain ||
        !p->backing->release || !p->backing->finish || !p->backing->valid(p)) return false;
    for (unsigned i=0;i<RECLAIM_QUEUE_SIZE;i++) if (!modified[i].page) {
        if (next_ticket==UINT32_MAX) { ledger_reclaim_account(0,0,0,1); return false; } /* never reuse a ticket */
        p->backing->retain(p);
        p->queued=true; d->io_in_flight=1; d->pins++;
        modified[i].page=p;
        modified[i].io=(struct reclaim_io){.ticket=++next_ticket,.phys=p->phys,
            .generation=p->generation,.dirty=d->dirty_epoch!=0,.prewrite=prewrite,
            .epoch=d->dirty_epoch};
        modified[i].taken=modified[i].completing=false;
        modified[i].dirty_epoch=d->dirty_epoch;
        if (prewrite) prewrite_queued++;
        if (pager_wake) pager_wake(); /* nonblocking deferred wake */
        return true;
    }
    ledger_reclaim_account(0,0,0,1); return false;
}
bool reclaim_writeback_take_class(struct reclaim_io *out, bool prewrite)
{
    if (!out) return false;
    uint32_t f=irq_save();
    for (unsigned i=0;i<RECLAIM_QUEUE_SIZE;i++) if (modified[i].page && !modified[i].taken &&
        modified[i].io.prewrite==prewrite) {
        modified[i].taken=true; *out=modified[i].io; irq_restore(f); return true;
    }
    irq_restore(f); return false;
}
bool reclaim_writeback_take(struct reclaim_io *out)
{ return reclaim_writeback_take_class(out,false) || reclaim_writeback_take_class(out,true); }
bool reclaim_writeback_waiting(void)
{
    uint32_t f=irq_save(); bool waiting=false;
    for (unsigned i=0;i<RECLAIM_QUEUE_SIZE && !waiting;i++) waiting=modified[i].page && !modified[i].taken;
    irq_restore(f); return waiting;
}
static unsigned entry_of(const struct reclaim_io *io)
{
    unsigned i;
    for (i=0;i<RECLAIM_QUEUE_SIZE;i++) if (modified[i].page && modified[i].taken &&
        modified[i].io.ticket==io->ticket && modified[i].io.generation==io->generation &&
        modified[i].io.phys==io->phys) break;
    return i;
}
bool reclaim_writeback_live(const struct reclaim_io *io)
{
    if (!io) return false;
    uint32_t f=irq_save(); bool live=entry_of(io)<RECLAIM_QUEUE_SIZE; irq_restore(f);
    return live;
}
bool reclaim_writeback_prepare(struct reclaim_io *io, void *buffer)
{
    if (!io) return false;
    uint32_t f=irq_save(); unsigned i=entry_of(io);
    if (i==RECLAIM_QUEUE_SIZE || modified[i].completing) { irq_restore(f); return false; }
    struct reclaim_page *p=modified[i].page;
    modified[i].completing=true; work_active++;
    irq_restore(f);
    io->outcome=RECLAIM_WRITE;
    /* Task context, no yield: the provider snapshot folds dirty bits first,
     * so the redirty check starts from exactly the copied version. */
    if (p->backing->prepare) p->backing->prepare(p,io,buffer);
    struct mm_page_descriptor *d=pmm_descriptor(p->phys);
    f=irq_save();
    modified[i].dirty_epoch=io->epoch=d->dirty_epoch;
    modified[i].io.outcome=io->outcome; modified[i].io.slot=io->slot; modified[i].io.epoch=io->epoch;
    modified[i].completing=false; work_active--;
    irq_restore(f);
    return true;
}
bool reclaim_writeback_complete(const struct reclaim_io *io, bool success)
{
    if (!io) return false;
    uint32_t f=irq_save(); unsigned i=entry_of(io);
    if (i==RECLAIM_QUEUE_SIZE || modified[i].completing) { irq_restore(f); return false; }
    struct reclaim_page *p=modified[i].page;
    if (p->generation!=io->generation) { irq_restore(f); return false; }
    modified[i].completing=true;
    work_active++;
    irq_restore(f);
    /* Provider revokes aliases in bounded steps while the queue reference
     * and pin still own the frame. May require repeated completion calls. */
    struct mm_page_descriptor *d=pmm_descriptor(p->phys);
    bool clean=success && d->dirty_epoch==modified[i].dirty_epoch;
    struct reclaim_io copy=modified[i].io;
    copy.slot=io->slot; copy.written=io->written; copy.outcome=io->outcome;
    bool done=p->backing->finish(p,&copy,clean);
    f=irq_save(); modified[i].completing=false;
    if (!done) { work_active--; irq_restore(f); return false; }
    /* finish may fold a later dirty bit while revoking aliases: never clear
     * a version newer than the one written. */
    if (clean && d->dirty_epoch==modified[i].dirty_epoch) d->dirty_epoch=0;
    d->io_in_flight=0; d->pins--; p->queued=false;
    if (modified[i].io.prewrite && prewrite_queued) prewrite_queued--;
    modified[i].page=0;
    /* Keep final publication and reference drop indivisible: otherwise a
     * scanner could requeue the node between clearing queued and unlinking
     * it in the provider's final frame_put. release cannot sleep or walk
     * aliases; finish already performed that bounded work in task context. */
    bool freed=p->backing->release(p); /* final free only after alias invalidation */
    if (freed) { struct reclaim_result r={.reclaimed=1}; ledger_reclaim_account(&r,0,0,0); }
    work_active--;
    struct mm_ledger memory; ledger_snapshot(&memory);
    if (active && !memory.pressure && !work_active) end_episode(&memory);
    irq_restore(f);
    return true;
}
static void age_step(void)
{
    struct mm_page_descriptor *d=pmm_descriptor(age_pfn*PAGE_SIZE);
    if (d && !pmm_page_free(age_pfn)) {
        unsigned age=(d->reserved>>7)&3u;
        if (d->reserved&MM_PAGE_TOUCHED) age=2;
        else if (age) age--;
        d->reserved=(d->reserved&~(MM_PAGE_TOUCHED|0x180u))|(age<<7);
        if (ledger_page_activity) ledger_page_activity(age_pfn*PAGE_SIZE,age!=0);
    }
    if (++age_pfn>=pmm_descriptor_count()) { age_pfn=0; phase=4; resident_cursor=resident_head; }
}
void reclaim_slice(void)
{
    /* Reclaim is what allocation waits for: never nested in pager I/O. */
    if (pager_order_check && !pager_order_check(PAGER_ORDER_ALLOC,(uintptr_t)__builtin_return_address(0))) return;
    uint64_t start=reclaim_clock();
    uint32_t start_flags=irq_save();
    struct mm_ledger memory; ledger_snapshot(&memory);
    if (!memory.pressure) {
        if (active && !work_active) end_episode(&memory);
        irq_restore(start_flags); return;
    }
    work_active++;
    if (!active) {
        episode_start=memory; active=true; phase=age_pfn=0;
        ledger_reclaim_account(0,1,0,0);
    }
    irq_restore(start_flags);
    struct { uint32_t id, pages; } owners_used[RECLAIM_SLICE_PAGES];
    unsigned owner_count=0;
    struct reclaim_result sum={0};
    /* 500 us dispatch guard leaves 500 us for the final bounded step and
     * snapshot. Every empty translation/descriptor/cursor advance counts. */
    unsigned limit=g_tsc_per_ms?RECLAIM_SLICE_PAGES:1u;
    for (unsigned steps=0;steps<limit && reclaim_us(start)<500;steps++) {
        if (!ledger_pressure()) break;
        struct reclaim_result r={.scanned=1};
        if (phase<2) {
            uint32_t f=irq_save(); struct block_cache *cache=reclaim_cache;
            cache_in_use=cache!=0; irq_restore(f);
            if (cache) cache_reclaim_step(cache,phase==0,&r);
            else r.done=true;
            f=irq_save(); cache_in_use=false; irq_restore(f);
            /* Empty/floor cache steps still consume the global work bound. */
            if (!r.scanned) r.scanned=1;
            if (r.done) { phase++; if (phase==2 && &vmm_sample_reset) vmm_sample_reset(); }
        } else {
            uint32_t f=irq_save();
            if (phase==2) {
                if (!vmm_sample_step || vmm_sample_step(epoch)) phase=3;
            } else if (phase==3) age_step();
            else {
                struct reclaim_page *p=resident_cursor;
                if (!p) { phase=0; epoch=epoch==UINT32_MAX?1:epoch+1; }
                else {
                    resident_cursor=p->next;
                    struct mm_page_descriptor *d=pmm_descriptor(p->phys);
                    if (d) {
                        if (d->pins) r.pinned++;
                        if (d->io_in_flight || p->queued) r.inflight++;
                        if (d->dirty_epoch) r.dirty++;
                        if (d->reserved&MM_PAGE_HOT) p->chances=2;
                        else if (p->chances) p->chances--;
                        else if (!d->pins && !d->io_in_flight && !p->queued) {
                            unsigned owner=0;
                            while (owner<owner_count && owners_used[owner].id!=p->owner_id) owner++;
                            if (owner==owner_count) { owners_used[owner].id=p->owner_id; owners_used[owner].pages=0; owner_count++; }
                            if (owners_used[owner].pages<RECLAIM_OWNER_LIMIT && queue_page(p,d,false)) owners_used[owner].pages++;
                        }
                    }
                }
            }
            irq_restore(f);
        }
        sum.scanned+=r.scanned; sum.reclaimed+=r.reclaimed;
        sum.dirty+=r.dirty; sum.inflight+=r.inflight; sum.pinned+=r.pinned;
    }
    ledger_reclaim_account(&sum,0,0,0);
    uint32_t elapsed=reclaim_us(start);
    uint32_t f=irq_save();
    work_active--;
    if (elapsed>pending.max_us) pending.max_us=elapsed;
    ledger_snapshot(&memory);
    if (active && !memory.pressure && !work_active) end_episode(&memory);
    irq_restore(f);
}
/* Idle prewrite (M4): same bounded slice rules, separate cursor, no cache
 * phase and no frame release. The provider samples the candidate's own
 * aliases (second chance) so aging progresses without a pressure episode. */
unsigned reclaim_prewrite_slice(unsigned limit)
{
    uint64_t start=reclaim_clock();
    unsigned queued=0, budget=g_tsc_per_ms?RECLAIM_SLICE_PAGES:1u;
    for (unsigned steps=0;steps<budget && reclaim_us(start)<500;steps++) {
        uint32_t f=irq_save();
        if (prewrite_queued>=limit || ledger_pressure()) { irq_restore(f); break; }
        struct reclaim_page *p=prewrite_cursor;
        if (!p) { prewrite_cursor=resident_head; irq_restore(f); if (!resident_head) break; continue; }
        prewrite_cursor=p->next;
        struct mm_page_descriptor *d=pmm_descriptor(p->phys);
        if (d && !d->pins && !d->io_in_flight && !p->queued && p->backing &&
            p->backing->prewrite && p->backing->prewrite(p) && queue_page(p,d,true)) queued++;
        irq_restore(f);
    }
    return queued;
}
static bool pressure_ready(void *arg) { (void)arg; return ledger_pressure(); }
/* f0-49: a task waiting for a frame (committed memory) asks for slices
 * back to back for 100 ms instead of one per 10 ms; each slice is still
 * bounded and followed by a safe point (R1). */
static volatile uint64_t hurry_until;
void reclaim_hurry(void)
{
    uint64_t until=g_ticks+100;
    if (until>hurry_until) hurry_until=until;
    if (worker_online) kwait_wake_irq(&pressure_wait,false);
}
static void reclaim_worker(void *arg)
{
    (void)arg;
    for (;;) {
        kwait_wait_until(&pressure_wait,pressure_ready,0,deadline_after_ms(1000));
        if (ledger_pressure()) {
            reclaim_slice();
            if (g_ticks<hurry_until) kpreempt_point(); else task_sleep_ms(10);
        }
    }
}
void reclaim_init(void)
{
    kwait_init(&pressure_wait);
    struct task *t=task_create_kernel("reclaim",reclaim_worker,0,P_NORMAL);
    if (!t) { klog("[mem] reclaim worker unavailable"); return; }
    worker_online=true; task_start(t);
}
