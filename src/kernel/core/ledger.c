/* M1 accounting. All mutations use the uniprocessor PMM irq exclusion;
 * no callback, object walk, allocation, I/O or sleep occurs under that lock.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/ledger.h>
#include <ciuki/task.h>
#include <ciuki/reclaim.h>

#define PIN_SLOTS 64u
struct pin_slot {
    struct mm_pin pin;
    enum mm_owner owner;
    bool live, retired;
    uint32_t min_pfn, max_pfn;
};
static struct {
    struct mm_ledger value;
    struct pin_slot pins[PIN_SLOTS];
    struct mm_ledger profile[3];
    uint64_t profile_tick[3];
    uint32_t ticket, sequence, critical, irq_context, profiles, pin_updates;
    const struct task *critical_task;
    uint64_t desktop_due;
    uint32_t record_id;
    bool audio_active, context_reported;
    uint64_t last_record, last_pressure, last_context;
    uint32_t reported_crossings, reported_mismatches;
} ledger;
_Static_assert(sizeof(ledger) <= 16384, "M1 kernel data budget");

static unsigned zone(uint32_t pfn) { return pfn < 256 ? ZONE_LOW : pfn < 4096 ? ZONE_DMA : ZONE_NORMAL; }
static const char *const owners[MM_OWNERS] = {
    "reserved", "image", "kernel", "heap", "stack", "tables", "dma", "device",
    "cache_data", "cache_meta", "cache_work", "user_private", "user_shared", "vm_private", "vm_pool", "io_request", "pfn_descriptors",
    "pager", "net"
};
static void pressure_update(void)
{
    struct mm_ledger *v = &ledger.value;
    uint32_t free = v->zone[ZONE_NORMAL].free;
    bool next = v->pressure ? free <= v->high : free < v->low;
    if (next != v->pressure) { v->pressure = next; v->pressure_crossings++;
        if (reclaim_wake) reclaim_wake(); }
    ledger.sequence++;
}
void ledger_init(void) { memset(&ledger, 0, sizeof(ledger)); }
void ledger_boot_page(uint32_t pfn, enum mm_owner owner, bool free)
{
    struct mm_zone_ledger *z = &ledger.value.zone[zone(pfn)];
    z->usable++;
    if (free) z->free++;
    else {
        z->resident[owner]++;
        ledger.value.cold[owner]++;
        struct mm_page_descriptor *d=pmm_descriptor?pmm_descriptor(pfn*PAGE_SIZE):0;
        if (d) d->reserved=owner;
        if (owner == MM_RESERVED || owner == MM_KERNEL_IMAGE || owner == MM_PFN_DESCRIPTORS) z->reserved++;
    }
}
void ledger_boot_done(void)
{
    uint32_t low = ledger.value.zone[ZONE_NORMAL].usable / 32;
    if (low < 1024) low = 1024;
    ledger.value.low = low; ledger.value.high = 2 * low;
    pressure_update();
}
void ledger_alloc_page(uint32_t pfn)
{
    struct mm_zone_ledger *z = &ledger.value.zone[zone(pfn)];
    z->free--; z->resident[MM_KERNEL]++; pressure_update();
    ledger.value.cold[MM_KERNEL]++;
    struct mm_page_descriptor *d=pmm_descriptor?pmm_descriptor(pfn*PAGE_SIZE):0;
    if (d) d->reserved=MM_KERNEL;
}
void ledger_free_page(uint32_t pfn)
{
    struct mm_zone_ledger *z = &ledger.value.zone[zone(pfn)];
    if (!z->resident[MM_KERNEL]) ledger.value.mismatches++;
    else z->resident[MM_KERNEL]--;
    z->free++; pressure_update();
    struct mm_page_descriptor *d=pmm_descriptor?pmm_descriptor(pfn*PAGE_SIZE):0;
    uint32_t *temperature=d && (d->reserved&MM_PAGE_HOT)?ledger.value.hot:ledger.value.cold;
    if (temperature[MM_KERNEL]) temperature[MM_KERNEL]--;
}
void ledger_move(uint32_t phys, uint32_t pages, enum mm_owner from, enum mm_owner to)
{
    uint32_t f = irq_save();
    if ((unsigned)from >= MM_OWNERS || (unsigned)to >= MM_OWNERS || !pages || (phys & 4095) ||
        phys / PAGE_SIZE >= PMM_MAX_PAGES || pages > PMM_MAX_PAGES - phys / PAGE_SIZE) {
        ledger.value.mismatches++; irq_restore(f); return;
    }
    uint32_t counts[3]={0}, first=phys/PAGE_SIZE, end=first+pages;
    static const uint32_t lo[3]={0,256,4096}, hi[3]={256,4096,PMM_MAX_PAGES};
    for (unsigned z=0;z<3;z++) {
        uint32_t a=first>lo[z] ? first : lo[z], b=end<hi[z] ? end : hi[z];
        if (b>a) counts[z]=b-a;
    }
    for (unsigned z = 0; z < 3; z++) if (ledger.value.zone[z].resident[from] < counts[z]) {
        ledger.value.mismatches++; irq_restore(f); return;
    }
    for (uint32_t p=first;p<end;) {
        uint32_t limit=end-p>32?p+32:end;
        for (;p<limit;p++) {
            struct mm_page_descriptor *d=pmm_descriptor?pmm_descriptor(p*PAGE_SIZE):0;
            bool hot=d && (d->reserved&MM_PAGE_HOT);
            uint32_t *temperature=hot?ledger.value.hot:ledger.value.cold;
            if (temperature[from]) temperature[from]--;
            /* Releasing to the allocator also drops its activity history. */
            bool keep_hot=hot && to!=MM_KERNEL;
            (keep_hot?ledger.value.hot:ledger.value.cold)[to]++;
            if (d) d->reserved=(d->reserved&~(MM_PAGE_OWNER_MASK|MM_PAGE_HOT))|to|(keep_hot?MM_PAGE_HOT:0);
            ledger.value.zone[zone(p)].resident[from]--;
            ledger.value.zone[zone(p)].resident[to]++;
        }
        ledger.sequence++;
        irq_restore(f); f=irq_save();
    }
    ledger.sequence++;
    irq_restore(f);
}
void ledger_page_activity(uint32_t phys, bool hot)
{
    uint32_t f=irq_save();
    struct mm_page_descriptor *d=pmm_descriptor?pmm_descriptor(phys):0;
    if (d && !pmm_page_free(phys/PAGE_SIZE)) {
        unsigned owner=d->reserved&MM_PAGE_OWNER_MASK;
        bool was=!!(d->reserved&MM_PAGE_HOT);
        if (owner<MM_OWNERS && was!=hot) {
            uint32_t *old=was?ledger.value.hot:ledger.value.cold;
            if (old[owner]) old[owner]--;
            (hot?ledger.value.hot:ledger.value.cold)[owner]++;
            d->reserved=(d->reserved&~MM_PAGE_HOT)|(hot?MM_PAGE_HOT:0);
        }
    }
    irq_restore(f);
}
void ledger_promise(enum mm_owner owner, int32_t reserved, int32_t committed)
{
    uint32_t f = irq_save();
    if ((unsigned)owner >= MM_OWNERS || (int64_t)ledger.value.reserved_va[owner] + reserved < 0 ||
        (int64_t)ledger.value.commit_ram[owner] + committed < 0 ||
        (int64_t)ledger.value.reserved_va[owner] + reserved > UINT32_MAX ||
        (int64_t)ledger.value.commit_ram[owner] + committed > UINT32_MAX) ledger.value.mismatches++;
    else { ledger.value.reserved_va[owner] += reserved; ledger.value.commit_ram[owner] += committed; }
    ledger.sequence++; irq_restore(f);
}
/* PMM exclusion held. RAM that can back promises, and all promises. */
static void commit_capacity(uint64_t *ram, uint64_t *promised_out)
{
    uint64_t free=0, resident=0, promised=0, pageable_commit=0;
    for (unsigned z=0;z<3;z++) {
        if (z!=ZONE_LOW) free+=ledger.value.zone[z].free;
        resident+=ledger.value.zone[z].resident[MM_USER_PRIVATE]+
                  ledger.value.zone[z].resident[MM_USER_SHARED]+
                  ledger.value.zone[z].resident[MM_VM_PRIVATE]+
                  ledger.value.zone[z].resident[MM_VM_POOL];
    }
    for (unsigned i=0;i<MM_OWNERS;i++) {
        uint32_t commit=ledger.value.commit_ram[i]; promised+=commit;
        if (i==MM_USER_PRIVATE || i==MM_USER_SHARED || i==MM_VM_PRIVATE || i==MM_VM_POOL)
            pageable_commit+=commit;
    }
    /* VM pool PFNs are covered by the existing VM_PRIVATE admission promise.
     * Other resident commitments (e.g. Win32 metadata) already occupy RAM;
     * subtract only their still-unmaterialized part, never charge twice. */
    if (resident>pageable_commit) resident=pageable_commit;
    for (unsigned i=0;i<MM_OWNERS;i++) {
        if (i==MM_USER_PRIVATE || i==MM_USER_SHARED || i==MM_VM_PRIVATE || i==MM_VM_POOL) continue;
        uint32_t pages=0;
        for (unsigned z=0;z<3;z++) pages+=ledger.value.zone[z].resident[i];
        resident+=pages<ledger.value.commit_ram[i]?pages:ledger.value.commit_ram[i];
    }
    *ram=free+resident; *promised_out=promised;
}
bool ledger_commit(enum mm_owner owner, uint32_t pages)
{
    uint32_t f=irq_save();
    uint64_t ram, promised; commit_capacity(&ram,&promised);
    /* Pagefile slots count only once published by the pager: extents are
     * allocated and flushed before admission may rely on them. */
    bool ok=(unsigned)owner<MM_OWNERS && pages<=INT32_MAX &&
            promised+pages<=ram+ledger.value.swap_capacity;
    if (ok) { ledger.value.commit_ram[owner]+=pages; ledger.sequence++; }
    irq_restore(f); return ok;
}
int64_t ledger_commit_headroom(void)
{
    uint32_t f=irq_save();
    uint64_t ram, promised; commit_capacity(&ram,&promised);
    int64_t headroom=(int64_t)(ram+ledger.value.swap_capacity)-(int64_t)promised;
    irq_restore(f); return headroom;
}
void ledger_swap(uint32_t capacity, uint32_t used)
{
    uint32_t f=irq_save();
    ledger.value.swap_capacity=capacity; ledger.value.swap_used=used; ledger.sequence++;
    irq_restore(f);
}
void ledger_mapping(int32_t mappings, int32_t shared)
{
    uint32_t f = irq_save();
    if ((int64_t)ledger.value.mappings + mappings < 0 || (int64_t)ledger.value.shared_mappings + shared < 0)
        ledger.value.mismatches++;
    else { ledger.value.mappings += mappings; ledger.value.shared_mappings += shared; }
    ledger.sequence++; irq_restore(f);
}
void ledger_snapshot(struct mm_ledger *out)
{
    uint32_t f = irq_save(); *out = ledger.value;
    /* Pageable promises beyond RAM backing: what the pagefile carries. */
    uint64_t ram, promised; commit_capacity(&ram,&promised);
    out->commit_pagefile = promised > ram ? (uint32_t)(promised - ram) : 0;
    irq_restore(f);
}
bool ledger_pressure(void)
{ uint32_t f = irq_save(); bool p = ledger.value.pressure; irq_restore(f); return p; }

bool ledger_reconcile(void)
{
    /* Static PMM bitmap only; interrupts can run between 32-PFN slices.
     * A mutation restarts the check, never produces a torn mismatch. */
    struct mm_ledger v;
    uint32_t free[3], seq;
    for (;;) {
        uint32_t f = irq_save(); seq = ledger.sequence; v = ledger.value; irq_restore(f);
        memset(free, 0, sizeof(free));
        bool retry = false;
        for (uint32_t p = 0; p < PMM_MAX_PAGES; p += 32) {
            f = irq_save();
            if (seq != ledger.sequence) retry = true;
            else for (unsigned i = 0; i < 32; i++) free[zone(p + i)] += pmm_page_free(p + i);
            irq_restore(f);
            if (retry) break;
        }
        f = irq_save(); retry |= seq != ledger.sequence;
        if (retry) { irq_restore(f); continue; }
        bool ok = true;
        for (unsigned z = 0; z < 3; z++) {
            uint64_t accounted = free[z];
            for (unsigned o = 0; o < MM_OWNERS; o++) accounted += v.zone[z].resident[o];
            if (accounted != v.zone[z].usable || free[z] != v.zone[z].free ||
                free[z] != pmm_zone_free(z) || v.zone[z].usable != pmm_zone_total(z)) ok = false;
        }
        for (unsigned o=0;o<MM_OWNERS;o++) {
            uint32_t resident=0;
            for (unsigned z=0;z<3;z++) resident+=v.zone[z].resident[o];
            if (v.hot[o]+v.cold[o]!=resident) ok=false;
        }
        if (!ok) { ledger.value.mismatches++; ledger.reported_mismatches=ledger.value.mismatches; }
        irq_restore(f);
        if (!ok) klog("[mem] ledger mismatch sequence=%u count=%u", seq, v.mismatches + 1);
        return ok;
    }
}

void ledger_reclaim_account(const struct reclaim_result *r,unsigned episodes,unsigned faults,unsigned stalls)
{
    uint32_t f=irq_save();
    ledger.value.reclaim_episodes+=episodes;
    ledger.value.reclaim_faults+=faults; ledger.value.reclaim_stalls+=stalls;
    if (r) {
        ledger.value.reclaim_scanned+=r->scanned; ledger.value.reclaim_reclaimed+=r->reclaimed;
        ledger.value.reclaim_dirty+=r->dirty; ledger.value.reclaim_inflight+=r->inflight;
        ledger.value.reclaim_pinned+=r->pinned;
    }
    irq_restore(f);
}
void ledger_reclaim_owner_delta(unsigned owner,int32_t delta)
{
    uint32_t f=irq_save();
    if (owner<MM_OWNERS) ledger.value.reclaim_owner_delta[owner]+=delta;
    irq_restore(f);
}

static uint32_t pin_pfn(const struct mm_pin *pin, uint32_t index)
{
    if (pin->pfn_table) {
        const uint32_t *pt = P2V(pin->pfn_table);
        return (pt[pin->first_entry + index] & ~4095u) / PAGE_SIZE;
    }
    return pin->first_pfn + index;
}
static bool registry_page_pinned(uint32_t pfn)
{
    for (unsigned i = 0; i < PIN_SLOTS; i++) {
        const struct pin_slot *s = &ledger.pins[i];
        if (!s->live) continue;
        if (s->pin.pfn_table / PAGE_SIZE == pfn && s->pin.pfn_table) return true;
        if (pfn < s->min_pfn || pfn > s->max_pfn) continue;
        if (!s->pin.pfn_table) return true;
        for (uint32_t j = 0; j < s->pin.pages; j++) if (pin_pfn(&s->pin,j) == pfn) return true;
    }
    return false;
}
bool ledger_page_pinned(uint32_t pfn)
{
    struct mm_page_descriptor *d=pmm_descriptor?pmm_descriptor(pfn*PAGE_SIZE):0;
    if (d && d->pins) return true;
    /* Completed registry tokens are fully represented in M2 descriptors.
     * The transition fallback preserves partial-publication protection. */
    if (d && !ledger.pin_updates) return false;
    return registry_page_pinned(pfn);
}
bool ledger_pin_transition(void) { return ledger.pin_updates!=0; }
static bool power2(uint32_t v) { return v && !(v & (v - 1)); }
int mm_pin(struct mm_pin *pin)
{
    if (!pin || !pin->generation || pin->ticket || !pin->pages || pin->first_pfn >= PMM_MAX_PAGES ||
        (pin->pfn_table ? pin->pages > 1024u || pin->first_entry > 1024u - pin->pages :
         pin->pages > PMM_MAX_PAGES - pin->first_pfn) || !power2(pin->alignment) ||
        (pin->boundary && !power2(pin->boundary)) || (unsigned)pin->direction > MM_PIN_BIDIRECTIONAL) return -EINVAL;
    if (pin->pfn_table && ((pin->pfn_table & 4095) || pin->pfn_table / PAGE_SIZE >= PMM_MAX_PAGES ||
        pmm_is_reserved(pin->pfn_table) || pmm_page_free(pin->pfn_table / PAGE_SIZE))) return -EINVAL;
    uint64_t start = (uint64_t)pin->first_pfn * PAGE_SIZE, end = start + (uint64_t)pin->pages * PAGE_SIZE;
    if (!pin->pfn_table && ((start & (pin->alignment - 1)) || ((end - 1) & ~(uint64_t)pin->address_mask) ||
        (pin->boundary && start / pin->boundary != (end - 1) / pin->boundary))) return -EINVAL;
    /* The caller owns a stable object throughout pin acquisition. Validate
     * in 32-page IRQ sections; publication retries if any owner/pin changes.
     * No pointer to caller storage survives the final registry copy. */
    for (;;) {
        uint32_t f=irq_save(), sequence=ledger.sequence;
        irq_restore(f);
        uint32_t min_pfn=PMM_MAX_PAGES, max_pfn=0;
        bool retry=false;
        for (uint32_t j=0;j<pin->pages;) {
            f=irq_save();
            if (sequence!=ledger.sequence) { irq_restore(f); retry=true; break; }
            uint32_t end_page=pin->pages-j>32 ? j+32 : pin->pages;
            for (;j<end_page;j++) {
                if (pin->pfn_table && !(((uint32_t *)P2V(pin->pfn_table))[pin->first_entry+j]&PTE_P)) {
                    irq_restore(f); return -EINVAL;
                }
                uint32_t p=pin_pfn(pin,j);
                if (p<min_pfn) min_pfn=p;
                if (p>max_pfn) max_pfn=p;
                if (p>=PMM_MAX_PAGES || p==pin->pfn_table/PAGE_SIZE ||
                    pmm_is_reserved(p*PAGE_SIZE) || pmm_page_free(p) || registry_page_pinned(p)) {
                    irq_restore(f); return -EINVAL;
                }
                uint32_t address=p*PAGE_SIZE;
                if (pin->pfn_table && (((address | (PAGE_SIZE-1)) & ~pin->address_mask) ||
                    (address & (pin->alignment-1)) || pin->boundary)) { irq_restore(f); return -EINVAL; }
            }
            irq_restore(f);
        }
        if (retry) continue;
        f=irq_save();
        if (sequence!=ledger.sequence) { irq_restore(f); continue; }
        unsigned slot=PIN_SLOTS;
        for (unsigned i=0;i<PIN_SLOTS;i++) if (!ledger.pins[i].live) { slot=i; break; }
        if (slot==PIN_SLOTS || ledger.ticket==UINT32_MAX) { irq_restore(f); return -ENOSPC; }
        pin->ticket=++ledger.ticket;
        ledger.pins[slot]=(struct pin_slot){.pin=*pin,.live=true,.min_pfn=min_pfn,.max_pfn=max_pfn};
        ledger.value.pin_tokens++; ledger.value.pinned_pages+=pin->pages+!!pin->pfn_table;
        ledger.pin_updates++;
        ledger.sequence++;
        irq_restore(f);
        for (uint32_t j=0;j<pin->pages;) {
            f=irq_save(); uint32_t end=pin->pages-j>32?j+32:pin->pages;
            for (;j<end;j++) if (pmm_descriptor) pmm_descriptor(pin_pfn(pin,j)*PAGE_SIZE)->pins++;
            irq_restore(f);
        }
        if (pin->pfn_table && pmm_descriptor) {
            f=irq_save(); pmm_descriptor(pin->pfn_table)->pins++; irq_restore(f);
        }
        f=irq_save(); ledger.pin_updates--; irq_restore(f);
        return 0;
    }
}

static struct pin_slot *find_pin(const struct mm_pin *pin, uint32_t generation)
{
    if (!pin || !pin->ticket || pin->generation != generation) return 0;
    for (unsigned i = 0; i < PIN_SLOTS; i++) {
        struct pin_slot *s = &ledger.pins[i];
        if (s->live && !memcmp(&s->pin, pin, sizeof(*pin))) return s;
    }
    return 0;
}
int mm_pin_retire(struct mm_pin *pin, uint32_t generation, enum mm_owner owner)
{
    uint32_t f = irq_save(); struct pin_slot *s = find_pin(pin, generation);
    if (!s || s->retired || s->pin.pfn_table || (unsigned)owner >= MM_OWNERS) { irq_restore(f); return -EINVAL; }
    s->retired = true; s->owner = owner;
    ledger.value.retired_pages += s->pin.pages; ledger.sequence++;
    irq_restore(f); return 0;
}
int mm_unpin(struct mm_pin *pin, uint32_t generation)
{
    uint32_t f = irq_save(); struct pin_slot *s = find_pin(pin, generation);
    if (!s) { ledger.value.double_unpins++; irq_restore(f); return -EINVAL; }
    struct pin_slot saved = *s; s->live = false; ledger.sequence++;
    ledger.pin_updates++;
    ledger.value.pin_tokens--; ledger.value.pinned_pages -= saved.pin.pages + !!saved.pin.pfn_table;
    if (saved.retired) ledger.value.retired_pages -= saved.pin.pages;
    /* Keep ticket in the caller: repeated completion is distinguishable from
     * a fresh zero-initialized token, including after a registry slot is reused. */
    irq_restore(f);
    for (uint32_t j=0;j<saved.pin.pages;) {
        f=irq_save(); uint32_t end=saved.pin.pages-j>32?j+32:saved.pin.pages;
        for (;j<end;j++) if (pmm_descriptor) pmm_descriptor(pin_pfn(&saved.pin,j)*PAGE_SIZE)->pins--;
        irq_restore(f);
    }
    if (saved.pin.pfn_table && pmm_descriptor) {
        f=irq_save(); pmm_descriptor(saved.pin.pfn_table)->pins--; irq_restore(f);
    }
    f=irq_save(); ledger.pin_updates--; irq_restore(f);
    if (saved.retired) for (uint32_t i = 0; i < saved.pin.pages; i++) {
        uint32_t phys = (saved.pin.first_pfn + i) * PAGE_SIZE;
        mm_release(phys, 1, saved.owner); pmm_free(phys);
    }
    return 0;
}
void ledger_critical(bool enter)
{
    uint32_t f=irq_save();
    if (enter) { ledger.critical_task=g_current; ledger.critical++; }
    else if (ledger.critical && ledger.critical_task==g_current) ledger.critical--;
    irq_restore(f);
}
void ledger_irq_context(bool enter)
{ if (enter) ledger.irq_context++; else if (ledger.irq_context) ledger.irq_context--; }
void ledger_assert_pinned_context(bool may_fault, uintptr_t site)
{
    uint32_t f=irq_save();
    if (!may_fault || !(ledger.irq_context || (ledger.critical && ledger.critical_task==g_current))) {
        irq_restore(f); return;
    }
    ledger.value.bad_context++;
    uint64_t now=g_ticks;
    bool report=!ledger.context_reported || now-ledger.last_context>=1000;
    uint32_t irq=ledger.irq_context;
    char task[sizeof(g_current->name)];
    if (report) {
        ledger.context_reported=true; ledger.last_context=now;
        if (g_current) memcpy(task,g_current->name,sizeof(task));
        else memcpy(task,"boot",sizeof("boot"));
        task[sizeof(task)-1]=0;
    }
    irq_restore(f);
    if (report) klog("[mem] pinned_context violation task=%s irq=%u site=%08x",task,irq,(uint32_t)site);
#ifdef CIUKI_LEDGER_STRICT
    panic("memory: reclaimable allocation/fault in pinned context");
#endif
}
static void record_snapshot(const char *profile,const struct mm_ledger *snapshot,uint64_t tick)
{
    struct mm_ledger v=*snapshot;
    uint32_t f=irq_save(); uint32_t id=++ledger.record_id; irq_restore(f);
    uint32_t reserve = 0, commit = 0;
    for (unsigned o = 0; o < MM_OWNERS; o++) { reserve += v.reserved_va[o]; commit += v.commit_ram[o]; }
    klog("[mem] ledger profile=%s snapshot=%u tick=%llu units=pages maps=%u shared_maps=%u",
         profile,id,tick,v.mappings,v.shared_mappings);
    klog("[mem] ledger snapshot=%u pins=%u pinned=%u retired=%u mismatches=%u double_unpins=%u bad_context=%u",
         id,v.pin_tokens,v.pinned_pages,v.retired_pages,v.mismatches,v.double_unpins,v.bad_context);
    klog("[mem] ledger snapshot=%u va_reserved=%u commit_ram=%u commit_pagefile=%u swap_capacity=%u swap_used=%u",
         id,reserve,commit,v.commit_pagefile,v.swap_capacity,v.swap_used);
    klog("[mem] reclaim snapshot=%u episodes=%u scanned=%u reclaimed=%u dirty=%u inflight=%u pinned=%u stalls=%u faults=%u",
         id,v.reclaim_episodes,v.reclaim_scanned,v.reclaim_reclaimed,v.reclaim_dirty,
         v.reclaim_inflight,v.reclaim_pinned,v.reclaim_stalls,v.reclaim_faults);
    for (unsigned o=0;o<MM_OWNERS;o++) if (v.hot[o] || v.cold[o])
        klog("[mem] activity snapshot=%u owner=%s hot=%u cold=%u trim_delta=%d",id,owners[o],v.hot[o],v.cold[o],v.reclaim_owner_delta[o]);
    for (unsigned o=0;o<MM_OWNERS;o++) if (v.reserved_va[o] || v.commit_ram[o])
        klog("[mem] ledger snapshot=%u owner=%s va_reserved=%u commit_ram=%u",
             id,owners[o],v.reserved_va[o],v.commit_ram[o]);
    for (unsigned z = 0; z < 3; z++) {
        klog("[mem] ledger snapshot=%u zone=%u usable=%u free=%u reserved=%u low=%u high=%u pressure=%u",
             id, z, v.zone[z].usable, v.zone[z].free, v.zone[z].reserved, z == ZONE_NORMAL ? v.low : 0,
             z == ZONE_NORMAL ? v.high : 0, v.pressure);
        for (unsigned o = 0; o < MM_OWNERS; o++) if (v.zone[z].resident[o])
            klog("[mem] ledger snapshot=%u zone=%u owner=%s resident=%u",
                 id, z, owners[o], v.zone[z].resident[o]);
    }
}
void ledger_record(const char *profile)
{
    struct mm_ledger v; uint32_t f=irq_save(); v=ledger.value; uint64_t tick=g_ticks; irq_restore(f);
    record_snapshot(profile,&v,tick);
}
/* Caller owns the PMM exclusion. Keep the first unprinted lifecycle point. */
static void queue_profile(unsigned index)
{
    if (!(ledger.profiles & (1u<<index))) {
        ledger.profile[index]=ledger.value; ledger.profile_tick[index]=g_ticks;
        ledger.profiles|=1u<<index;
    }
}

void ledger_poll(uint64_t now_ms)
{
    uint32_t f=irq_save();
    if (ledger.desktop_due && now_ms>=ledger.desktop_due) { queue_profile(0); ledger.desktop_due=0; }
    irq_restore(f);
    static const char *const names[]={"settled-desktop","audio-active","copy-active"};
    for (unsigned i=0;i<3;i++) {
        struct mm_ledger captured;
        f=irq_save(); bool pending=!!(ledger.profiles&(1u<<i));
        uint64_t tick=ledger.profile_tick[i];
        if (pending) { captured=ledger.profile[i]; ledger.profiles&=~(1u<<i); }
        irq_restore(f);
        if (pending) record_snapshot(names[i],&captured,tick);
    }
    struct mm_ledger v; ledger_snapshot(&v);
    if (v.mismatches!=ledger.reported_mismatches) {
        ledger.reported_mismatches=v.mismatches;
        klog("[mem] ledger mismatch accounting_hook count=%u",v.mismatches);
    }
    if (v.pressure_crossings != ledger.reported_crossings && now_ms - ledger.last_pressure >= 1000) {
        uint32_t crossings = v.pressure_crossings - ledger.reported_crossings;
        ledger.value.pressure_suppressed += crossings - 1;
        ledger.reported_crossings = v.pressure_crossings; ledger.last_pressure = now_ms;
        klog("[mem] pressure active=%u free=%u low=%u high=%u crossings=%u coalesced=%u dma_free=%u",
             v.pressure, v.zone[ZONE_NORMAL].free, v.low, v.high, v.pressure_crossings,
             ledger.value.pressure_suppressed, v.zone[ZONE_DMA].free);
    }
    if (now_ms - ledger.last_record >= 30000) { ledger.last_record = now_ms; ledger_record("periodic"); }
}
static bool contains(const char *s,size_t n,const char *needle)
{
    size_t k=strlen(needle);
    for (size_t i=0;k<=n && i<=n-k;i++) if (!memcmp(s+i,needle,k)) return true;
    return false;
}
void ledger_lifecycle(const char *s, size_t n)
{
    bool desktop=contains(s,n,"[desktop] stage=first_frame_presented");
    bool copy=contains(s,n,"[files] job=copy start ");
    if (!desktop && !copy) return;
    uint32_t f=irq_save();
    if (desktop && !ledger.desktop_due) ledger.desktop_due=g_ticks+5000;
    if (copy) queue_profile(2);
    irq_restore(f);
}

void ledger_audio_active(bool active)
{
    uint32_t f=irq_save();
    if (active && !ledger.audio_active) queue_profile(1);
    ledger.audio_active=active;
    irq_restore(f);
}
