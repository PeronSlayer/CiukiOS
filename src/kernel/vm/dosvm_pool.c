/* f3-46 (contract D1): the per-VM XMS/EMS/DPMI pool as one pageable memory
 * object on the f0-47 model (committed slots, frames only while resident,
 * PFN descriptors carrying access/dirty history for every alias), the f0-48
 * global replacement (a reclaim_backing provider) and the f0-49 backing
 * store (pager slots, verified page-in). Research, sources and decisions:
 * docs/design/dos-dpmi-contract.md, "Pageable pool and protected I/O".
 *
 * Blocking rules. A frame is materialized or paged in only in task context
 * with interrupts enabled: at a guest #PF before any emulation (the trap's
 * published continuation), in a kernel transfer before its first byte, in
 * the prefault of an instruction's memory before its device effect, in a
 * service validation, or at the trap's settle point (DMA pins). While the
 * worker is inside a monitor transaction (hold) this VM's pages are never
 * queued, revoked or detached by the pager, so a page resolved in that
 * transaction stays resident until it completes. Non-blocking callers (the
 * clock work's virtual DMA) see only resident frames.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/abi.h>
#include <ciuki/timing.h>
#include <ciuki/pager.h>
#include "dosvm_internal.h"

#define POOL_FRAME_TRIES 500u /* 10 ms each: the f0-49 fault path's 5 s */
#define POOL_BUSY_TRIES 5000u /* 1 ms each: another task's page-in */
#define POOL_SITE ((uintptr_t)__builtin_return_address(0))

/* One resident frame of a paged pool. It can outlive its page (and the VM)
 * while the reclaim queue holds it: close and discard orphan it (vm NULL)
 * and the queue's release frees it, exactly once. */
struct vm_frame {
    struct reclaim_page reclaim; /* phys: this frame */
    struct dosvm *vm;
    uint32_t index, swap;        /* pool page; clean pager copy while descriptor clean */
    uint16_t holds;              /* reclaim queue references */
    bool detach;                 /* finish moved the only copy to the page's slot */
};
static uint32_t pool_epoch;
static const struct reclaim_backing pool_provider;

static struct vm_page *page_at(const struct dosvm *v, uint32_t index)
{
    struct vm_page *block = P2V(v->memory.pool.dir[index / VM_POOL_PER_PAGE]);
    return &block[index % VM_POOL_PER_PAGE];
}
static uint32_t pool_va(uint32_t index) { return VM_POOL_BASE + index * PAGE_SIZE; }
static struct mm_page_descriptor *descriptor(uint32_t phys)
{ return phys && pmm_descriptor ? pmm_descriptor(phys & ~0xFFFu) : 0; }
/* Task context with physical IF set and no preemption hold. Read EFLAGS
 * directly: no IRQ-off section is opened on this hot path. */
static bool can_block(void)
{ return (read_eflags() & 0x200u) && !(g_current && g_current->preempt_holds); }
static uint32_t *pte_of(const struct dosvm *v, uint32_t va)
{
    uint32_t de = ((uint32_t *)P2V(v->as.pd_phys))[va >> 22];
    return de & PTE_P ? (uint32_t *)P2V(de & ~0xFFFu) + ((va >> 12) & 1023u) : 0;
}
/* Fold sampled A/D bits into the frame's descriptor: every alias of one
 * pool page contributes to the same history (f0-48 sampling semantics). */
static void fold(uint32_t bits)
{
    struct mm_page_descriptor *d = bits & PTE_P ? descriptor(bits) : 0;
    if (!d) return;
    if (bits & PTE_A) { d->accessed_epoch = ++pool_epoch; d->reserved |= MM_PAGE_TOUCHED; }
    if (bits & PTE_D) d->dirty_epoch = d->dirty_epoch == UINT32_MAX ? 1 : d->dirty_epoch + 1;
}
static void dirty(uint32_t phys)
{
    struct mm_page_descriptor *d = descriptor(phys);
    if (d) d->dirty_epoch = d->dirty_epoch == UINT32_MAX ? 1 : d->dirty_epoch + 1;
}
static void pte_set(struct dosvm *v, uint32_t va, uint32_t value)
{
    uint32_t *p = pte_of(v, va);
    if (!p) return; /* pool tables exist from creation; windows use the low table */
    uint32_t old = *p;
    if ((old & PTE_P) && (value & PTE_P) && !((old ^ value) & ~0xFFFu)) value |= old & (PTE_A | PTE_D);
    else fold(old);
    bool was = old & PTE_P, now = value & PTE_P;
    if (was != now) {
        if (now) { v->as.pages++; g_user_mappings++; }
        else { v->as.pages--; g_user_mappings--; }
        if (ledger_mapping) ledger_mapping(now ? 1 : -1, 0);
    }
    *p = value;
    if (old != value && read_cr3() == v->as.pd_phys) invlpg(va);
}
static bool pte_sample(struct dosvm *v, uint32_t va, bool clear_accessed)
{
    uint32_t *p = pte_of(v, va);
    if (!p || !(*p & PTE_P)) return false;
    bool accessed = *p & PTE_A;
    uint32_t clear = PTE_D | (clear_accessed ? PTE_A : 0), bits = *p & clear;
    if (bits) {
        fold((*p & ~0xFFFu) | PTE_P | bits);
        *p &= ~clear;
        if (read_cr3() == v->as.pd_phys) invlpg(va);
    }
    return accessed;
}
enum alias_op { ALIAS_PUBLISH, ALIAS_REVOKE, ALIAS_SAMPLE, ALIAS_SAMPLE_ACCESS };
/* The pool linear page and each EMS frame window naming it. Bounded:
 * one pool PTE plus sixteen window PTEs, no allocation, no yield. */
static bool aliases(struct dosvm *v, uint32_t index, enum alias_op op, uint32_t phys)
{
    /* Borrowed LFB PTEs never enter the pool sampler or publication path.
     * The original RAM frame remains pinned/charged until copy-back. */
    if (v->vbe_direct.pages && pool_va(index)-v->vbe_direct.base<v->vbe_direct.pages*PAGE_SIZE) return false;
    struct vm_pool *pool = &v->memory.pool;
    bool accessed = false;
    for (int i = -1; i < (int)VM_POOL_WINDOWS; i++) {
        uint32_t va;
        if (i < 0) va = pool_va(index);
        else if (pool->window[i] == index + 1) va = pool->window_base + (uint32_t)i * PAGE_SIZE;
        else continue;
        if (op == ALIAS_PUBLISH) pte_set(v, va, phys | PTE_P | PTE_U | PTE_W);
        else if (op == ALIAS_REVOKE) {
            uint32_t *p = pte_of(v, va);
            if (p && (*p & (PTE_P | PTE_A)) == (PTE_P | PTE_A)) accessed = true;
            pte_set(v, va, 0);
        } else accessed |= pte_sample(v, va, op == ALIAS_SAMPLE_ACCESS);
    }
    return accessed;
}
static int pool_index(const struct dosvm *v, uint32_t linear, uint32_t *index)
{
    const struct vm_pool *pool = &v->memory.pool;
    if (!pool->pages) return -ENOENT;
    if (linear >= VM_POOL_BASE) {
        uint32_t k = (linear - VM_POOL_BASE) / PAGE_SIZE;
        if (k >= pool->pages) return -EFAULT;
        *index = k;
        return 0;
    }
    if (linear >= pool->window_base && linear - pool->window_base < VM_POOL_WINDOWS * PAGE_SIZE) {
        uint32_t w = pool->window[(linear - pool->window_base) / PAGE_SIZE];
        if (!w) return -ENOENT; /* unmapped window: an absent low page, as before */
        *index = w - 1;
        return 0;
    }
    return -ENOENT;
}

/* Free a frame that left its page, or orphan it to the reclaim queue. */
static void frame_drop(struct vm_frame *f, uint32_t phys)
{
    if (!f) { mm_free(phys, MM_VM_POOL); return; }
    uint32_t flags = irq_save();
    if (f->holds) { f->vm = 0; irq_restore(flags); return; }
    if (reclaim_untrack) reclaim_untrack(&f->reclaim);
    uint32_t swap = f->swap;
    f->swap = 0;
    irq_restore(flags);
    if (swap && pager_slot_free) pager_slot_free(swap);
    mm_free(phys, MM_VM_POOL);
    kfree(f);
}

/* New frame for a ZERO or SWAPPED page. Allocation (which may wait for
 * reclaim) precedes the busy mark: a wait never freezes this page's object
 * against the pager (the f0-49 resolve_page order). -EAGAIN: raced, retry. */
static int materialize(struct dosvm *v, uint32_t index)
{
    struct vm_pool *pool = &v->memory.pool;
    struct vm_page *page = page_at(v, index);
    uint32_t generation = v->info.key.generation;
    if (pager_order_check && !pager_order_check(PAGER_ORDER_ALLOC, POOL_SITE)) return -EDEADLK;
    struct vm_frame *f = 0;
    if (!pool->locked && !(f = kmalloc(sizeof(*f)))) return -ENOMEM;
    uint32_t phys = 0;
    for (unsigned tries = 0;; tries++) {
        phys = pmm_alloc_zone(ZONE_NORMAL);
        if (phys || tries >= POOL_FRAME_TRIES || !(pager_online && pager_online()) || !can_block()) break;
        if (reclaim_hurry) reclaim_hurry(); else if (reclaim_wake) reclaim_wake();
        if (pager_wake) pager_wake();
        pool->stats.frame_waits++;
        task_sleep_ms(10);
        if (pool->closing || v->info.key.generation != generation) break;
    }
    bool gone = pool->closing || v->info.key.generation != generation;
    if (!phys || gone || page->state == VM_PAGE_RESIDENT || (page->flags & VM_PAGE_BUSY)) {
        if (phys) pmm_free(phys);
        kfree(f);
        return gone ? -EFAULT : phys ? -EAGAIN : -ENOMEM;
    }
    mm_claim(phys, 1, MM_VM_POOL);
    if (pager_order_enter && !pager_order_enter(PAGER_ORDER_OBJECT, POOL_SITE)) {
        mm_free(phys, MM_VM_POOL); kfree(f); return -EDEADLK;
    }
    page->flags |= VM_PAGE_BUSY;
    bool swapped = page->state == VM_PAGE_SWAPPED;
    struct mm_page_descriptor *d = descriptor(phys);
    if (d) d->io_in_flight = 1;
    int e = 0;
    if (swapped) {
        /* Verified page-in into the unpublished frame, or a contained error:
         * never zeros, never another owner's bytes. Faults coalesce on busy. */
        e = pager_page_in ? pager_page_in(page->swap, P2V(phys)) : -EIO;
        if (e && e != -EDEADLK) e = -EIO;
        if (!e && (pool->closing || v->info.key.generation != generation)) e = -EFAULT;
    } else memset(P2V(phys), 0, PAGE_SIZE);
    if (d) d->io_in_flight = 0;
    if (e) {
        if (e == -EIO) pool->stats.page_in_errors++;
        mm_free(phys, MM_VM_POOL);
        kfree(f);
    } else {
        uint32_t keep = 0;
        if (swapped) {
            if (f && pager_keep_copy && pager_keep_copy()) keep = page->swap;
            else if (pager_slot_free) pager_slot_free(page->swap);
            page->swap = 0; pool->swapped--; pool->stats.page_ins++;
            if (d) d->dirty_epoch = 0; /* a kept copy matches until written */
        } else pool->stats.zero_fills++;
        if (d) { d->backing_kind = MM_BACKING_PRIVATE; d->backing_slot = index; }
        page->phys = phys; page->frame = f; page->state = VM_PAGE_RESIDENT;
        pool->resident++;
        v->info.resident += PAGE_SIZE;
        if (f) {
            *f = (struct vm_frame){.reclaim = {.phys = phys, .generation = ++pool_epoch,
                .owner = MM_VM_POOL, .owner_id = v->as.pd_phys, .backing = &pool_provider},
                .vm = v, .index = index, .swap = keep};
            if (reclaim_track) reclaim_track(&f->reclaim);
        }
        aliases(v, index, ALIAS_PUBLISH, phys);
    }
    page->flags &= ~VM_PAGE_BUSY;
    if (pager_order_exit) pager_order_exit(PAGER_ORDER_OBJECT);
    return e;
}

/* Make one page resident (wait permitting) and record the access. */
static int resolve(struct dosvm *v, uint32_t index, bool write, bool wait, bool publish)
{
    /* Cached resolutions occur once per page in guest transfers. Yield
     * before borrowing any frame/PTE, including the already-resident path;
     * a large resident range must not run as one unbroken monitor slice.
     * Clock/DMA callers (wait=false) retain their nonblocking semantics. */
    struct vm_pool *pool = &v->memory.pool;
    uint32_t generation = v->info.key.generation;
    if (wait && can_block()) kpreempt_point();
    if (pool->closing || v->info.key.generation!=generation || index>=pool->pages) return -EFAULT;
    struct vm_page *page = page_at(v, index);
    uint64_t start = 0;
    bool worker = g_current && g_current == v->task;
    int e = 0;
    for (unsigned attempt = 0;; attempt++) {
        if (pool->closing || v->info.key.generation != generation || index>=pool->pages) { e = -EFAULT; break; }
        page=page_at(v,index); /* revalidate the directory after a preemption/retry */
        if (page->state == VM_PAGE_RESIDENT) break;
        if (!wait || !can_block()) { e = -EAGAIN; break; }
        if (!start) {
            start = dosvm_perf_clock() | 1;
            if (worker) { pool->waiting = true; if (pool->effects) pool->stats.late_waits++; }
        }
        if (page->flags & VM_PAGE_BUSY) {
            if (attempt >= POOL_BUSY_TRIES) { e = -EBUSY; break; }
            pool->stats.busy_waits++;
            task_sleep_ms(1);
            continue;
        }
        e = materialize(v, index);
        if (e && e != -EAGAIN) break;
        e = 0;
        if (can_block()) kpreempt_point(); /* retry a raced publication fairly */
    }
    if (start) {
        uint64_t now = dosvm_perf_clock(), spent = now > start ? now - start : 0;
        uint32_t us = g_tsc_per_ms ? (uint32_t)(spent * 1000u / g_tsc_per_ms) : 0;
        pool->stats.waits++; pool->stats.wait_us += us;
        if (us > pool->stats.max_wait_us) pool->stats.max_wait_us = us;
        if (worker) pool->waiting = false;
    }
    if (e) return e;
    struct mm_page_descriptor *d = descriptor(page->phys);
    if (d) {
        d->accessed_epoch = ++pool_epoch; d->reserved |= MM_PAGE_TOUCHED;
        if (write) dirty(page->phys);
    }
    /* A page-out in flight may have revoked translations; the frame is
     * still ours. finish rechecks the dirty version after revoking again. */
    if (publish) aliases(v, index, ALIAS_PUBLISH, page->phys);
    return 0;
}

int dosvm_pool_frame(struct dosvm *v, uint32_t linear, bool write, bool wait, uint32_t *phys)
{
    uint32_t index;
    int e = pool_index(v, linear, &index);
    if (e) return e;
    e = resolve(v, index, write, wait, false);
    if (!e) *phys = page_at(v, index)->phys | (linear & 0xFFFu);
    return e;
}
bool dosvm_pool_range(const struct dosvm *v, uint32_t linear, bool *alias)
{
    uint32_t index;
    if (alias) *alias = linear < VM_POOL_BASE;
    return !pool_index(v, linear, &index);
}
/* Guest not-present fault at the trap's entry, before any emulation: the
 * faulting instruction restarts after publication. -ENOENT: not a pool page. */
int dosvm_pool_fault(struct dosvm *v, uint32_t linear, uint32_t error)
{
    uint32_t index;
    if (error & 1u) return -ENOENT;
    if (pool_index(v, linear, &index)) return -ENOENT;
    return resolve(v, index, error & 2u, true, true);
}
/* Make a range resident before an instruction's device effect. */
int dosvm_pool_prefault(struct dosvm *v, uint32_t linear, uint32_t bytes, bool write)
{
    if (!bytes) return 0;
    uint32_t end = linear + bytes - 1;
    if (end < linear) return -EFAULT;
    for (uint32_t va = linear & ~0xFFFu;; va += PAGE_SIZE) {
        uint32_t index;
        int e = pool_index(v, va, &index);
        if (e == -EFAULT) return e;
        if (!e && (e = resolve(v, index, write, true, true))) return e;
        if (va >= (end & ~0xFFFu)) return 0;
    }
}

int dosvm_pool_resident(struct dosvm *v, uint32_t linear, uint32_t bytes)
{
    if (!bytes) return 0;
    uint32_t end = linear + bytes - 1;
    if (end < linear) return -EFAULT;
    for (uint32_t va = linear & ~0xFFFu;; va += PAGE_SIZE) {
        uint32_t index;
        if (!pool_index(v, va, &index) && page_at(v, index)->state != VM_PAGE_RESIDENT) return -EAGAIN;
        if (va >= (end & ~0xFFFu)) return 0;
    }
}
/* EMS frame window page w (0..15) aliases pool_linear's page, or nothing.
 * The old translation's A/D history is folded before it is replaced. */
void dosvm_pool_window(struct dosvm *v, unsigned w, uint32_t pool_linear)
{
    struct vm_pool *pool = &v->memory.pool;
    if (w >= VM_POOL_WINDOWS) return;
    uint32_t value = 0, index;
    pool->window[w] = 0;
    if (pool_linear >= VM_POOL_BASE && !pool_index(v, pool_linear, &index)) {
        struct vm_page *page = page_at(v, index);
        pool->window[w] = index + 1;
        if (page->state == VM_PAGE_RESIDENT) value = page->phys | PTE_P | PTE_U | PTE_W;
    }
    pte_set(v, pool->window_base + w * PAGE_SIZE, value);
}

static void wait_unbusy(struct dosvm *v, struct vm_page *page)
{
    for (unsigned i = 0; (page->flags & VM_PAGE_BUSY) && i < POOL_BUSY_TRIES && can_block(); i++) {
        v->memory.pool.stats.busy_waits++;
        task_sleep_ms(1);
    }
}
/* Allocation-time zeroing (XMS/EMS/DPMI/VBE) and DPMI 0703h: a pinned or
 * locked-mode page is cleared in place; any other page returns to
 * demand-zero, freeing its frame (or orphaning it to an in-flight
 * page-out) and its pager slot. Commitment is unchanged. */
static void discard(struct dosvm *v, uint32_t index)
{
    struct vm_pool *pool = &v->memory.pool;
    struct vm_page *page = page_at(v, index);
    wait_unbusy(v, page);
    if (page->flags & VM_PAGE_BUSY) return; /* cannot happen in the worker */
    if (page->state == VM_PAGE_RESIDENT && (pool->locked || page->pins)) {
        memset(P2V(page->phys), 0, PAGE_SIZE);
        dirty(page->phys);
        return;
    }
    if (page->state == VM_PAGE_RESIDENT) {
        aliases(v, index, ALIAS_REVOKE, 0);
        struct vm_frame *f = page->frame;
        uint32_t phys = page->phys;
        page->frame = 0; page->phys = 0; page->state = VM_PAGE_ZERO;
        pool->resident--;
        v->info.resident -= PAGE_SIZE;
        frame_drop(f, phys);
    } else if (page->state == VM_PAGE_SWAPPED) {
        if (pager_slot_free) pager_slot_free(page->swap);
        page->swap = 0; page->state = VM_PAGE_ZERO; pool->swapped--;
    } else return;
    pool->stats.discards++;
}
void dosvm_pool_zero(struct dosvm *v, uint32_t linear, uint32_t pages)
{
    uint32_t index;
    for (uint32_t i = 0; i < pages; i++)
        if (!pool_index(v, linear + i * PAGE_SIZE, &index)) discard(v, index);
}
/* DPMI 0702h: advisory demand-paging candidate. Drop the access history
 * so the pages are replaced ahead of recently used ones; contents stay. */
void dosvm_pool_advise(struct dosvm *v, uint32_t linear, uint32_t pages)
{
    uint32_t index;
    for (uint32_t i = 0; i < pages; i++) {
        if (pool_index(v, linear + i * PAGE_SIZE, &index)) continue;
        struct vm_page *page = page_at(v, index);
        if (page->state != VM_PAGE_RESIDENT || !page->frame) continue;
        aliases(v, index, ALIAS_SAMPLE_ACCESS, 0);
        struct mm_page_descriptor *d = descriptor(page->phys);
        uint32_t flags = irq_save();
        page->frame->reclaim.chances = 0;
        if (d) d->reserved &= ~(MM_PAGE_TOUCHED | 0x180u);
        irq_restore(flags);
        if (d && ledger_page_activity) ledger_page_activity(page->phys, false);
    }
}

static void unpin_page(struct dosvm *v, uint32_t index)
{
    struct vm_page *page = page_at(v, index);
    if (!page->pins) return;
    struct mm_page_descriptor *d = descriptor(page->phys);
    if (d && d->pins) d->pins--;
    if (!--page->pins) v->memory.pool.pinned--;
}
static int pin_page(struct dosvm *v, uint32_t index)
{
    struct vm_page *page = page_at(v, index);
    if (page->pins == UINT16_MAX) return -EBUSY;
    /* Resident and translated before the device/model may use it. */
    int e = resolve(v, index, false, true, true);
    if (e) return e;
    if (!page->pins++) v->memory.pool.pinned++;
    struct mm_page_descriptor *d = descriptor(page->phys);
    if (d) d->pins++;
    return 0;
}
/* Pins cover whole pool pages in [linear, linear + pages * 4 KiB); low
 * pages are always resident and need none. All or nothing. */
int dosvm_pool_pin(struct dosvm *v, uint32_t linear, uint32_t pages)
{
    uint32_t index;
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t va = (linear & ~0xFFFu) + i * PAGE_SIZE;
        if (va < VM_POOL_BASE || pool_index(v, va, &index)) continue;
        int e = pin_page(v, index);
        if (e) {
            v->memory.pool.stats.pin_failures++;
            dosvm_pool_unpin(v, linear, i);
            return e;
        }
    }
    return 0;
}
void dosvm_pool_unpin(struct dosvm *v, uint32_t linear, uint32_t pages)
{
    uint32_t index;
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t va = (linear & ~0xFFFu) + i * PAGE_SIZE;
        if (va >= VM_POOL_BASE && !pool_index(v, va, &index)) unpin_page(v, index);
    }
}
void dosvm_pool_hold(struct dosvm *v, bool enter)
{
    struct vm_pool *pool = &v->memory.pool;
    if (enter) pool->hold++;
    else if (pool->hold && !--pool->hold) pool->effects = 0;
}
/* Virtual DMA: pin each unmasked channel's 64/128 KiB window where it
 * overlaps the pool, at the trap's settle point (the OUT that unmasked or
 * reprogrammed it has completed). New pins precede the old ones' release;
 * a channel masked by terminal count is released at the next settle. */
void dosvm_pool_settle(struct dosvm *v)
{
    struct vm_pool *pool = &v->memory.pool;
    if (!pool->dma_dirty || !pool->pages) return;
    pool->dma_dirty = false;
    for (unsigned ch = 0; ch < VM_POOL_DMA; ch++) {
        uint32_t linear = 0, bytes = 0, first = 0, count = 0;
        if (dosvm_dma_window(v, ch, &linear, &bytes)) {
            uint32_t start = linear > VM_POOL_BASE ? linear : VM_POOL_BASE;
            uint32_t end = linear + bytes, top = VM_POOL_BASE + pool->pages * PAGE_SIZE;
            if (end > top) end = top;
            if (end > start) { first = start; count = (end - start) / PAGE_SIZE; }
        }
        if (first == pool->dma_first[ch] && count == pool->dma_pages[ch]) continue;
        uint32_t old_first = pool->dma_first[ch], old_count = pool->dma_pages[ch];
        if (count && dosvm_pool_pin(v, first, count)) count = 0; /* reads see -EAGAIN */
        pool->dma_first[ch] = count ? first : 0; pool->dma_pages[ch] = count;
        if (old_count) dosvm_pool_unpin(v, old_first, old_count);
    }
}
/* Clock-work sample per elapsed tick (no blocking): classify a pending
 * guest IRQ as page-in wait, guest virtual IF clear, or host descheduling. */
void dosvm_pool_irq_sample(struct dosvm *v, uint64_t elapsed)
{
    if (!elapsed || dosvm_pic_pending(&v->monitor) < 0) return;
    struct vm_pool *pool = &v->memory.pool;
    if (pool->waiting) pool->stats.irq_page_ms += elapsed;
    else if (!v->monitor.vif) pool->stats.irq_guest_if_ms += elapsed;
    else if (g_current != v->task) pool->stats.irq_host_ms += elapsed;
}

int dosvm_pool_init(struct dosvm *v, bool locked)
{
    struct vm_pool *pool = &v->memory.pool;
    memset(pool, 0, sizeof(*pool));
    pool->locked = locked;
    pool->window_base = (uint32_t)v->memory.frame_segment * 16u;
    uint32_t pages = v->memory.quota / PAGE_SIZE;
    if (!pages) return 0;
    uint32_t dir = (pages + VM_POOL_PER_PAGE - 1) / VM_POOL_PER_PAGE;
    for (uint32_t i = 0; i < dir; i++) {
        uint32_t phys = pmm_alloc_zone(ZONE_NORMAL);
        if (!phys) return -ENOMEM;
        mm_claim(phys, 1, MM_VM_PRIVATE);
        memset(P2V(phys), 0, PAGE_SIZE);
        pool->dir[i] = phys; pool->dir_pages++;
        v->info.resident += PAGE_SIZE;
        kpreempt_point(); /* record page published; nothing else held */
    }
    /* Every pool page table exists from creation: faults, pins and the
     * pager only rewrite PTEs, never allocate a table. */
    uint32_t *pd = P2V(v->as.pd_phys);
    for (uint32_t va = VM_POOL_BASE & ~0x3FFFFFu; va < VM_POOL_BASE + pages * PAGE_SIZE; va += 0x400000u) {
        if (pd[va >> 22] & PTE_P) continue;
        uint32_t table = pmm_alloc();
        if (!table) return -ENOMEM;
        mm_claim(table, 1, MM_TABLE);
        memset(P2V(table), 0, PAGE_SIZE);
        pd[va >> 22] = table | PTE_P | PTE_W | PTE_U;
        v->as.tables++;
        v->info.resident += PAGE_SIZE;
        kpreempt_point(); /* table published and accounted */
    }
    /* Commitment: the quota moves from the VM's admission promise to the
     * pool. Locked: RAM-backed (eager). Paged: RAM or published pagefile
     * capacity (M1/M4), growing the pagefile first when needed. */
    if (ledger_promise) ledger_promise(MM_VM_PRIVATE, -(int32_t)pages, -(int32_t)pages);
    bool ok = true;
    if (locked || !ledger_commit) {
        if (ledger_promise) ledger_promise(MM_VM_POOL, (int32_t)pages, (int32_t)pages);
    } else {
        ok = ledger_commit(MM_VM_POOL, pages) ||
             (pager_commit_wait && pager_commit_wait(pages) && ledger_commit(MM_VM_POOL, pages));
        if (ok && ledger_promise) ledger_promise(MM_VM_POOL, (int32_t)pages, 0);
    }
    if (!ok) {
        if (ledger_promise) ledger_promise(MM_VM_PRIVATE, (int32_t)pages, (int32_t)pages);
        return -ENOMEM;
    }
    pool->commit = pool->reserve = pages;
    pool->committed = true;
    pool->pages = pages;
    return 0;
}
/* Locked mode: adopt the eager loop's mapped frames (dosvm_memory_init). */
void dosvm_pool_adopt(struct dosvm *v)
{
    struct vm_pool *pool = &v->memory.pool;
    for (uint32_t i = 0; i < pool->pages; i++) {
        struct vm_page *page = page_at(v, i);
        uint32_t phys = as_lookup(&v->as, pool_va(i), 0) & ~0xFFFu;
        if (!phys || page->state != VM_PAGE_ZERO) continue;
        page->phys = phys; page->state = VM_PAGE_RESIDENT;
        pool->resident++;
        if (!((i + 1) & 63u)) kpreempt_point();
    }
}

/* Single teardown: revoke every alias, return pins, frames (orphaning
 * queued ones to the reclaim queue), slots, commitment and record pages.
 * Bounded slices of 16 pages; the worker has exited. */
int dosvm_pool_close(struct dosvm *v)
{
    struct vm_pool *pool = &v->memory.pool;
    pool->closing = true;
    for (uint32_t i = 0; i < pool->pages; i++) {
        struct vm_page *page = page_at(v, i);
        wait_unbusy(v, page);
        if (page->state == VM_PAGE_RESIDENT) {
            aliases(v, i, ALIAS_REVOKE, 0);
            struct mm_page_descriptor *d = descriptor(page->phys);
            if (d) d->pins = d->pins > page->pins ? d->pins - page->pins : 0;
            if (page->pins) pool->pinned--;
            uint32_t phys = page->phys;
            struct vm_frame *f = page->frame;
            *page = (struct vm_page){0};
            pool->resident--;
            v->info.resident -= PAGE_SIZE;
            frame_drop(f, phys);
        } else if (page->state == VM_PAGE_SWAPPED) {
            if (pager_slot_free) pager_slot_free(page->swap);
            pool->swapped--;
        }
        *page = (struct vm_page){0};
        if (!((i + 1) & 15u)) kpreempt_point();
    }
    for (unsigned w = 0; w < VM_POOL_WINDOWS; w++) if (pool->window[w]) {
        pool->window[w] = 0;
        pte_set(v, pool->window_base + w * PAGE_SIZE, 0);
    }
    if (pool->committed && ledger_promise) {
        ledger_promise(MM_VM_POOL, -(int32_t)pool->reserve, -(int32_t)pool->commit);
        ledger_promise(MM_VM_PRIVATE, (int32_t)pool->commit, (int32_t)pool->commit);
    }
    pool->committed = false; pool->commit = pool->reserve = 0;
    for (uint32_t i = 0; i < pool->dir_pages; i++) {
        mm_free(pool->dir[i], MM_VM_PRIVATE);
        pool->dir[i] = 0;
        v->info.resident -= PAGE_SIZE;
    }
    pool->dir_pages = pool->pages = 0;
    pool->resident = pool->swapped = pool->pinned = 0;
    memset(pool->dma_first, 0, sizeof(pool->dma_first));
    memset(pool->dma_pages, 0, sizeof(pool->dma_pages));
    return 0;
}

/* ---- f0-48/f0-49 provider. valid/retain/release/prewrite run under IRQ
 * exclusion, bounded and nonblocking; prepare/finish in pager task context
 * without yielding. A pinned, busy, locked or in-transaction page is never
 * selected, revoked or detached. ---- */
static struct vm_frame *frame_of(struct reclaim_page *p)
{ return (struct vm_frame *)(void *)((char *)p - offsetof(struct vm_frame, reclaim)); }
static struct vm_page *frame_page(struct vm_frame *f)
{
    struct dosvm *v = f->vm;
    if (!v || v->memory.pool.closing || v->memory.pool.locked || f->index >= v->memory.pool.pages) return 0;
    struct vm_page *page = page_at(v, f->index);
    return page->frame == f && page->state == VM_PAGE_RESIDENT ? page : 0;
}
static bool copy_valid(struct vm_frame *f)
{
    struct mm_page_descriptor *d = descriptor(f->reclaim.phys);
    return f->swap && d && !d->dirty_epoch && pager_slot_readable && pager_slot_readable(f->swap);
}
static bool selectable(struct vm_frame *f, struct vm_page *page)
{ return page && !page->pins && !(page->flags & VM_PAGE_BUSY) && !f->vm->memory.pool.hold; }
static bool frame_valid(struct reclaim_page *p)
{
    struct vm_frame *f = frame_of(p);
    if (!selectable(f, frame_page(f))) return false;
    return copy_valid(f) || f->swap || (pager_can_reserve && pager_can_reserve());
}
static void frame_retain(struct reclaim_page *p) { frame_of(p)->holds++; }
static bool frame_release(struct reclaim_page *p)
{
    struct vm_frame *f = frame_of(p);
    if (f->holds) f->holds--;
    if (f->holds || (f->vm && !f->detach)) return false;
    if (reclaim_untrack) reclaim_untrack(p);
    if (f->swap && pager_slot_free) pager_slot_free(f->swap);
    mm_free(p->phys, MM_VM_POOL);
    kfree(f);
    return true;
}
static bool frame_prewrite(struct reclaim_page *p)
{
    struct vm_frame *f = frame_of(p);
    if (!selectable(f, frame_page(f))) return false;
    if (aliases(f->vm, f->index, ALIAS_SAMPLE_ACCESS, 0)) { p->chances = 2; return false; }
    if (p->chances) { p->chances--; return false; }
    return !copy_valid(f);
}
static void frame_prepare(struct reclaim_page *p, struct reclaim_io *io, void *buffer)
{
    struct vm_frame *f = frame_of(p);
    if (!f->vm) { io->outcome = RECLAIM_GONE; return; }
    struct vm_page *page = frame_page(f);
    struct mm_page_descriptor *d = descriptor(p->phys);
    if (!selectable(f, page) || !d || d->pins > 1) { io->outcome = RECLAIM_DEFER; return; }
    /* Page-out revokes every alias first (folding A/D); a racing access
     * faults and republishes the still-owned frame, which finish detects. */
    aliases(f->vm, f->index, io->prewrite ? ALIAS_SAMPLE : ALIAS_REVOKE, 0);
    io->slot = f->swap;
    if (copy_valid(f)) { io->outcome = RECLAIM_CLEAN; return; }
    if (!buffer) { io->outcome = RECLAIM_DEFER; return; }
    memcpy(buffer, P2V(p->phys), PAGE_SIZE);
    io->outcome = RECLAIM_WRITE;
}
static bool frame_finish(struct reclaim_page *p, const struct reclaim_io *io, bool clean)
{
    struct vm_frame *f = frame_of(p);
    bool fresh = io->outcome == RECLAIM_WRITE && io->slot && io->slot != f->swap;
    struct dosvm *v = f->vm;
    struct vm_page *page = v ? frame_page(f) : 0;
    if (!page || io->outcome == RECLAIM_GONE || io->outcome == RECLAIM_DEFER) {
        if (fresh && pager_slot_free) pager_slot_free(io->slot);
        if (v && io->outcome == RECLAIM_DEFER) v->memory.pool.stats.deferred++;
        return true;
    }
    struct vm_pool *pool = &v->memory.pool;
    struct mm_page_descriptor *d = descriptor(p->phys);
    if (io->outcome == RECLAIM_WRITE) {
        if (io->written) {
            /* Adopt the copy; it stays valid only while the page is clean. */
            if (fresh) { if (f->swap && pager_slot_free) pager_slot_free(f->swap); f->swap = io->slot; }
            if (pager_slot_commit) pager_slot_commit(f->swap);
        } else {
            if (fresh && pager_slot_free) pager_slot_free(io->slot);
            else if (io->slot && !(pager_slot_readable && pager_slot_readable(io->slot))) f->swap = 0;
            return true; /* the resident page and its dirty version stay intact */
        }
    }
    if (!clean) { pool->stats.redirtied++; return true; }
    if (io->prewrite) { aliases(v, f->index, ALIAS_SAMPLE, 0); pool->stats.prewrites++; return true; }
    /* A transaction or a pin since queueing keeps the frame (the written
     * slot remains a clean copy); never revoke a pinned translation. */
    if (pool->hold || page->pins || !d || d->pins > 1) { pool->stats.redirtied++; return true; }
    aliases(v, f->index, ALIAS_REVOKE, 0);
    if (d->dirty_epoch != io->epoch || !f->swap || !(pager_slot_readable && pager_slot_readable(f->swap))) {
        pool->stats.redirtied++;
        return true;
    }
    /* The slot is now the only copy: detach; release frees the PFN after
     * the queue drops its pin, every alias already invalidated above. */
    page->frame = 0; page->phys = 0; page->state = VM_PAGE_SWAPPED;
    page->swap = f->swap; f->swap = 0; f->detach = true;
    pool->resident--; pool->swapped++;
    v->info.resident -= PAGE_SIZE;
    if (io->outcome == RECLAIM_CLEAN) pool->stats.clean_discards++;
    else pool->stats.pageouts++;
    return true;
}
static const struct reclaim_backing pool_provider = {frame_valid, frame_retain, frame_release,
    frame_finish, frame_prepare, frame_prewrite};
