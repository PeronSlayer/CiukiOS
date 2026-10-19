/* Transactional user VA extents and eagerly reserved private backing.
 * PTE_OWN retains a frame even when PROT_NONE clears Present. F0 page tables
 * never contain this bit and remain owned by core/vmm.c.
 * Research/decisions: build/f2-02/research.md; execution-abi.md, F2 memory.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/process.h>
#include <ciuki/cpustat.h>

extern int w32_view_access(struct uaddr *,uint32_t,bool) __attribute__((weak));

#define PTE_OWN 0x200u
#define PTE_NEW 0x400u
#define PTE_SHARED 0x800u
static uint64_t next_generation;
/* Read/mutate metadata in bounded slices. A stack-owned mutation pin keeps
 * existing extent nodes alive across service; disjoint new maps may still be
 * inserted. Never retain a raw list predecessor over an unreserved yield.
 * 128 cached extent visits * 1 us = 128 us; the counter spans nested walks. */
static unsigned walk_work;
static void walk_hold(const struct uaddr *u, struct ua_pin *pin)
{
    struct uaddr *owner=(struct uaddr *)u;
    *pin=(struct ua_pin){.base=0,.end=CIUKI_MAIN_STACK_LIMIT,.mutation=true,
                         .task=g_current,.next=owner->pins};
    owner->pins=pin;
}
static void walk_step(void)
{ if (!(++walk_work & 127u)) kpreempt_point(); }
static void walk_drop(const struct uaddr *u, struct ua_pin *pin)
{ ua_unpin((struct uaddr *)u,pin); }

static bool span(uint32_t base, uint32_t bytes, uint32_t *end)
{
    if (!bytes || (base & (PAGE_SIZE - 1)) || bytes > UINT32_MAX - (PAGE_SIZE - 1))
        return false;
    uint32_t rounded = PAGE_ALIGN_UP(bytes);
    if (base >= CIUKI_MAIN_STACK_LIMIT || rounded > CIUKI_MAIN_STACK_LIMIT - base)
        return false;
    *end = base + rounded;
    return true;
}

int ua_protection(uint32_t prot)
{
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
        return -EINVAL;
    if (prot != PROT_NONE && prot != PROT_READ && prot != (PROT_READ | PROT_WRITE) &&
        prot != (PROT_READ | PROT_EXEC))
        return -EACCES;
    return 0;
}

static uint32_t *pte(struct uaddr *u, uint32_t va, bool create)
{
    uint32_t *pd = P2V(u->as.pd_phys);
    uint32_t *de = &pd[va >> 22];
    if (!(*de & PTE_P)) {
        if (!create)
            return 0;
        uint32_t phys = pmm_alloc();
        if (!phys)
            return 0;
        mm_claim(phys,1,MM_TABLE);
        memset(P2V(phys), 0, PAGE_SIZE);
        *de = phys | PTE_P | PTE_W | PTE_U;
        u->as.tables++;
    }
    uint32_t *pt = P2V(*de & ~(PAGE_SIZE - 1));
    return &pt[(va >> 12) & 1023];
}

static void flush(struct uaddr *u, uint32_t va)
{
    if (read_cr3() == u->as.pd_phys)
        invlpg(va);
}

static void set_pte(struct uaddr *u, uint32_t va, uint32_t *p, uint32_t value)
{
    bool was = !!(*p & PTE_P), now = !!(value & PTE_P);
    if (was != now) {
        if (now) { u->as.pages++; g_user_mappings++; }
        else { u->as.pages--; g_user_mappings--; }
    }
    if (ledger_mapping) ledger_mapping((int)now-(int)was,
        (int)(now && (value & PTE_SHARED))-(int)(was && (*p & PTE_SHARED)));
    *p = value;
    flush(u, va);
}

static void free_pages(struct uaddr *u, uint32_t base, uint32_t end, bool only_new)
{
    for (uint32_t va = base; va < end; va += PAGE_SIZE) {
        /* 16 * (6 us release + 1 us check) <=112 us, including holes.
         * No spinlock/IRQ hold; caller reserves the changing extent. */
        if (!(((va - base) / PAGE_SIZE) & 15u)) kpreempt_point();
        uint32_t *p = pte(u, va, false);
        if (!p || (only_new && (!(*p & PTE_NEW) || (*p & PTE_SHARED))))
            continue;
        if (*p & PTE_OWN) {
            if (!(*p & PTE_SHARED))
                mm_free(*p & ~(PAGE_SIZE - 1), MM_USER_PRIVATE);
            if (!(*p & PTE_SHARED) && ledger_promise) ledger_promise(MM_USER_PRIVATE,0,-1);
            u->backing--;
        }
        set_pte(u, va, p, 0);
    }
    /* Empty page tables are reclaimed on rollback and on unmap, including
     * provisional tables created for a PROT_NONE reservation. */
    uint32_t *pd = P2V(u->as.pd_phys);
    for (uint32_t i = base >> 22; i <= (end - 1) >> 22; i++) {
        kpreempt_point();
        if (!(pd[i] & PTE_P))
            continue;
        uint32_t *pt = P2V(pd[i] & ~(PAGE_SIZE - 1));
        unsigned j = 0;
        /* Indivisible emptiness test: 1024 * 0.25 us = 256 us.
         * A yield here could free pt or populate an already checked entry. */
        while (j < 1024 && !pt[j]) j++;
        if (j == 1024) {
            mm_free(pd[i] & ~(PAGE_SIZE - 1), MM_TABLE);
            pd[i] = 0;
            u->as.tables--;
        }
    }
}

static int reserve_pages(struct uaddr *u, uint32_t base, uint32_t end)
{
    for (uint32_t va = base; va < end; va += PAGE_SIZE) {
        uint32_t *p = pte(u, va, true);
        if (!p)
            goto fail;
        if (*p & PTE_OWN) {
            if (((va - base) / PAGE_SIZE + 1) % 16 == 0) kpreempt_point();
            continue;
        }
        uint32_t phys = pmm_alloc();
        if (!phys)
            goto fail;
        mm_claim(phys,1,MM_USER_PRIVATE);
        if (ledger_promise) ledger_promise(MM_USER_PRIVATE,0,1);
        memset(P2V(phys), 0, PAGE_SIZE);
        *p = phys | PTE_OWN | PTE_NEW;
        u->backing++;
        if(u->backing>u->peak_backing)u->peak_backing=u->backing;
        if (((va - base) / PAGE_SIZE + 1) % 16 == 0) kpreempt_point();
    }
    return 0;
fail:
    free_pages(u, base, end, true);
    return -ENOMEM;
}

static void protect_pages(struct uaddr *u, uint32_t base, uint32_t end, uint32_t prot)
{
    for (uint32_t va = base; va < end; va += PAGE_SIZE) {
        /* 16 PTE updates * 6 us = 96 us, counting reserved holes. */
        if (!(((va - base) / PAGE_SIZE) & 15u)) kpreempt_point();
        uint32_t *p = pte(u, va, false);
        if (!p || !(*p & PTE_OWN))
            continue;
        /* Shared+NEW denotes a stable COW alias; NEW alone is provisional
         * private backing. Never expose shared COW pages as writable, even
         * during a yielding VirtualProtect transaction. */
        bool cow=(*p&PTE_SHARED)&&(*p&PTE_NEW);
        uint32_t value = (*p & ~(PAGE_SIZE - 1)) | PTE_OWN | (*p & PTE_SHARED) | (cow?PTE_NEW:0);
        if (prot != PROT_NONE)
            value |= PTE_P | PTE_U | ((prot & PROT_WRITE) && !cow ? PTE_W : 0);
        set_pte(u, va, p, value);
    }
}

int ua_init(struct uaddr *u, void *identity)
{
    memset(u, 0, sizeof(*u));
    u->identity = identity;
    return as_create(&u->as);
}

struct ua_extent *ua_find(const struct uaddr *u, uint32_t va)
{
    struct ua_pin walk; walk_hold(u,&walk);
    struct ua_extent *found=0;
    for (struct ua_extent *e = u->head; e && e->base <= va; e = e->next) {
        walk_step();
        if (va < e->end) { found=e; break; }
    }
    walk_drop(u,&walk); return found;
}

static bool arena(const struct ua_extent *e)
{
    return e->kind != UA_W32 && e->base >= CIUKI_MMAP_BASE && e->end <= CIUKI_MMAP_LIMIT;
}

static bool vacant(const struct uaddr *u, uint32_t base, uint32_t end)
{
    struct ua_pin walk; walk_hold(u,&walk); bool free=true;
    for (const struct ua_extent *e = u->head; e; e = e->next) {
        walk_step();
        if (base < e->end && end > e->base) { free=false; break; }
    }
    walk_drop(u,&walk); return free;
}

int ua_map_at(struct uaddr *u, uint32_t base, uint32_t bytes, uint32_t prot,
              uint32_t maximum, enum ua_kind kind, uint32_t owner)
{
    uint32_t end;
    int err = u->win32 && !(prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) ? 0 : ua_protection(prot);
    if (err)
        return err;
    if (!span(base, bytes, &end) || base < (u->win32 ? CIUKI_W32_LOW : CIUKI_IMAGE_BASE) ||
        (u->win32 && end > CIUKI_W32_SHARED + PAGE_SIZE))
        return -EINVAL;
    /* Metadata service pins serialize insertion too: otherwise a peer could
     * invalidate a prepared split's extent-count debit while its walker
     * yielded. Ordinary range pins retain disjoint mapping concurrency. */
    for (const struct ua_pin *pin=u->pins; pin; pin=pin->next)
        if (pin->mutation && base<pin->end && end>pin->base) return -EBUSY;
    if (!vacant(u,base,end)) return -EINVAL;
    bool in_arena = kind != UA_W32 && base >= CIUKI_MMAP_BASE && end <= CIUKI_MMAP_LIMIT;
    if ((in_arena && u->arena_extents == CIUKI_MAPPING_MAX) || next_generation == UINT64_MAX)
        return -ENOMEM;
    struct ua_extent *e = kzalloc(sizeof(*e));
    if (!e)
        return -ENOMEM;
    *e = (struct ua_extent){ .base = base, .end = end, .prot = PROT_NONE, .maximum = maximum,
        .kind = kind, .owner = owner, .generation = ++next_generation, .preparing = true };
    struct ua_pin walk; walk_hold(u,&walk);
    struct ua_extent **at = &u->head;
    while (*at && (*at)->base < base) { walk_step(); at = &(*at)->next; }
    e->next = *at; *at = e;
    walk_drop(u,&walk);
    u->extents++;
    if (in_arena) u->arena_extents++;
    uint64_t start = cpu_path_clock ? cpu_path_clock() : 0;
    if (prot && (err = reserve_pages(u, base, end))) {
        /* Other disjoint mappings may have changed the list while yielded. */
        walk_hold(u,&walk);
        at = &u->head;
        while (*at != e) { walk_step(); at = &(*at)->next; }
        *at = e->next; u->extents--;
        if (in_arena) u->arena_extents--;
        kfree(e);
        walk_drop(u,&walk);
        return err;
    }
    protect_pages(u, base, end, prot);
    if (ledger_promise) ledger_promise(kind==UA_SHARED ? MM_USER_SHARED : MM_USER_PRIVATE,(end-base)/PAGE_SIZE,0);
    e->prot = prot; e->preparing = false;
    if (cpu_path_add && cpu_path_clock) cpu_path_add(CPU_ZERO, cpu_path_clock() - start);
    return 0;
}

static uint32_t first_fit(const struct uaddr *u, uint32_t bytes)
{
    struct ua_pin walk; walk_hold(u,&walk);
    uint32_t base = CIUKI_MMAP_BASE;
    for (const struct ua_extent *e = u->head; e; e = e->next) {
        walk_step();
        if (e->end <= base)
            continue;
        if (e->base >= base && bytes <= e->base - base)
            break;
        base = e->end;
    }
    walk_drop(u,&walk);
    return base <= CIUKI_MMAP_LIMIT && bytes <= CIUKI_MMAP_LIMIT - base ? base : 0;
}

int32_t ua_mmap(struct uaddr *u, const struct ciuki_mmap_args *a)
{
    if (u->win32) return -EPERM;
    uint32_t known = MAP_PRIVATE | MAP_SHARED | MAP_FIXED | MAP_ANONYMOUS;
    if (a->size != sizeof(*a) || (a->flags & ~known) || !a->length ||
        a->length > UINT32_MAX - (PAGE_SIZE - 1) ||
        (a->flags & (MAP_PRIVATE | MAP_SHARED)) == (MAP_PRIVATE | MAP_SHARED) ||
        !(a->flags & (MAP_PRIVATE | MAP_SHARED)))
        return -EINVAL;
    if ((a->flags & (MAP_FIXED | MAP_SHARED)) || !(a->flags & MAP_ANONYMOUS))
        return -EOPNOTSUPP;
    if (a->fd != -1 || a->offset != 0)
        return -EINVAL;
    int err = ua_protection(a->prot);
    if (err)
        return err;
    uint32_t bytes = PAGE_ALIGN_UP(a->length), base = PAGE_ALIGN_DOWN(a->hint);
    if (bytes > CIUKI_MMAP_LIMIT - CIUKI_MMAP_BASE ||
        (a->hint && (base < CIUKI_MMAP_BASE || base >= CIUKI_MMAP_LIMIT ||
                    bytes > CIUKI_MMAP_LIMIT - base)))
        return -EINVAL;
    if (!a->hint || !vacant(u, base, base + bytes))
        base = first_fit(u, bytes);
    if (!base)
        return -ENOMEM;
    err = ua_map_at(u, base, bytes, a->prot, PROT_READ | PROT_WRITE | PROT_EXEC, UA_ANON, 0);
    return err ? err : (int32_t)base;
}

static bool pinned(const struct uaddr *u, uint32_t base, uint32_t end)
{
    struct ua_pin walk; walk_hold(u,&walk); bool busy=false;
    for (const struct ua_extent *e = u->head; e; e = e->next) {
        walk_step();
        if (e->preparing && base < e->end && end > e->base) { busy=true; break; }
    }
    /* Pins are stack-owned by active callers (<=16 process threads plus
     * bounded kernel callers); do not yield while following their links. */
    for (const struct ua_pin *p = walk.next; !busy && p; p = p->next)
        if (base < p->end && end > p->base) busy=true;
    walk_drop(u,&walk); return busy;
}

/* At most two new records: the boundaries of a single transaction. Allocate
 * them before touching either metadata or backing. No large kernel stack. */
static int prepare_split(struct uaddr *u, uint32_t base, uint32_t end, bool removing,
                         struct ua_extent **left, struct ua_extent **right)
{
    *left = *right = 0;
    struct ua_extent *a = ua_find(u, base), *b = ua_find(u, end - 1);
    bool l = a && base > a->base, r = b && end < b->end;
    int final = (int)u->arena_extents + (l && arena(a)) + (r && arena(b));
    if (removing) {
        struct ua_pin walk; walk_hold(u,&walk);
        for (struct ua_extent *e = u->head; e; e = e->next) {
            walk_step();
            if (arena(e) && e->base < end && e->end > base) final--;
        }
        walk_drop(u,&walk);
    }
    if (final > CIUKI_MAPPING_MAX)
        return -ENOMEM;
    if (l && !(*left = kmalloc(sizeof(**left))))
        return -ENOMEM;
    if (r && !(*right = kmalloc(sizeof(**right)))) {
        kfree(*left);
        *left = 0;
        return -ENOMEM;
    }
    return 0;
}

static void split(struct uaddr *u, uint32_t at, struct ua_extent *fresh)
{
    if (!fresh)
        return;
    struct ua_extent *e = ua_find(u, at);
    *fresh = *e;
    if (fresh->shared.object) fresh->shared.retain(fresh->shared.object);
    fresh->base = at;
    e->end = at;
    e->next = fresh;
    u->extents++;
    if (arena(fresh))
        u->arena_extents++;
}

static void remove_range(struct uaddr *u, uint32_t base, uint32_t end)
{
    struct ua_pin walk; walk_hold(u,&walk);
    struct ua_extent **at = &u->head;
    while (*at) {
        walk_step();
        struct ua_extent *e = *at;
        if (e->base >= base && e->end <= end) {
            if (ledger_promise) ledger_promise(e->kind==UA_SHARED ? MM_USER_SHARED : MM_USER_PRIVATE,-(int32_t)((e->end-e->base)/PAGE_SIZE),0);
            ww_cancel(u->identity, e->generation, e->base, e->end);
            e->preparing = true;
            free_pages(u, e->base, e->end, false);
            at = &u->head;
            while (*at != e) { walk_step(); at = &(*at)->next; }
            if (e->shared.object) e->shared.release(e->shared.object);
            *at = e->next;
            u->extents--;
            if (arena(e))
                u->arena_extents--;
            kfree(e);
        } else {
            at = &e->next;
        }
    }
    walk_drop(u,&walk);
}

static bool protected_kind(const struct ua_extent *e)
{
    return e->kind == UA_STACK || e->kind == UA_GUARD || e->kind == UA_TLS;
}

int ua_munmap(struct uaddr *u, uint32_t base, uint32_t bytes)
{
    uint32_t end;
    if (!span(base, bytes, &end) || base < CIUKI_MMAP_BASE || end > CIUKI_MMAP_LIMIT)
        return -EINVAL;
    if (pinned(u, base, end))
        return -EBUSY;
    struct ua_pin walk; walk_hold(u,&walk); bool protected=false;
    for (struct ua_extent *e = u->head; e; e = e->next) {
        walk_step();
        if (base < e->end && end > e->base && protected_kind(e)) { protected=true; break; }
    }
    walk_drop(u,&walk);
    if (protected) return -EBUSY;
    struct ua_extent *l, *r;
    int err = prepare_split(u, base, end, true, &l, &r);
    if (err)
        return err;
    split(u, base, l);
    split(u, end, r);
    remove_range(u, base, end);
    return 0;
}

int ua_mprotect(struct uaddr *u, uint32_t base, uint32_t bytes, uint32_t prot)
{
    uint32_t end;
    if (!span(base, bytes, &end) || base < CIUKI_IMAGE_BASE)
        return -EINVAL;
    int err = u->win32 && !(prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) ? 0 : ua_protection(prot);
    if (err)
        return err;
    if (pinned(u, base, end) || end > CIUKI_MAIN_STACK_RESERVATION_BASE)
        return -EBUSY;
    for (uint32_t va = base; va < end;) {
        struct ua_extent *e = ua_find(u, va);
        if (!e)
            return -ENOMEM;
        if (protected_kind(e))
            return -EBUSY;
        if (prot & ~e->maximum)
            return -EACCES;
        va = e->end < end ? e->end : end;
    }
    struct ua_extent *l, *r;
    err = prepare_split(u, base, end, false, &l, &r);
    if (err)
        return err;
    /* Retain metadata ownership through backing, protection and publication.
     * Whole-space mutation pins keep predecessor nodes and split debits
     * stable while page and extent walks service unrelated tasks. */
    struct ua_extent *first = ua_find(u, base), *last = ua_find(u, end - 1);
    struct ua_pin transaction = { .base = 0, .end = CIUKI_MAIN_STACK_LIMIT, .mutation = true, .next = u->pins };
    u->pins = &transaction;
    unsigned debit = (l && arena(first)) + (r && arena(last));
    u->arena_extents += debit;
    if (prot) err = reserve_pages(u, base, end);
    u->arena_extents -= debit;
    if (err) { ua_unpin(u, &transaction); kfree(l); kfree(r); return err; }
    split(u, base, l);
    split(u, end, r);
    protect_pages(u, base, end, prot);
    for (struct ua_extent *e = u->head; e; e = e->next) {
        walk_step();
        if (e->base >= base && e->end <= end) e->prot = prot;
    }
    ua_unpin(u, &transaction);
    return 0;
}

bool ua_range(const struct uaddr *u, uint32_t va, uint32_t bytes, uint32_t prot)
{
    if (!bytes)
        return true;
    if (va < (u->win32 ? CIUKI_W32_LOW : CIUKI_IMAGE_BASE) || va >= CIUKI_MAIN_STACK_LIMIT ||
        bytes > CIUKI_MAIN_STACK_LIMIT - va)
        return false;
    uint32_t end = va + bytes;
    while (va < end) {
        const struct ua_extent *e = ua_find(u, va);
        if (!e || e->preparing || e->kind == UA_GUARD || (e->prot & prot) != prot)
            return false;
        va = e->end < end ? e->end : end;
    }
    return true;
}

int ua_pin(struct uaddr *u, struct ua_pin *pin, uint32_t base, uint32_t bytes, bool write)
{
    if (!ua_range(u, base, bytes, PROT_READ | (write ? PROT_WRITE : 0)))
        return -EFAULT;
    for (struct ua_pin *p = u->pins; p; p = p->next)
        if (p->mutation && base < p->end && base + bytes > p->base) return -EBUSY;
    if (u->win32 && w32_view_access)
        for (uint32_t va=base&~4095u; va < (uint64_t)base+bytes; va+=PAGE_SIZE) {
            int err=w32_view_access(u,va,write); if(err)return err;
        }
    *pin = (struct ua_pin){ .base = base, .end = base + bytes, .task = g_current, .next = u->pins };
    u->pins = pin;
    return 0;
}

void ua_unpin(struct uaddr *u, struct ua_pin *pin)
{
    struct ua_pin **at = &u->pins;
    while (*at && *at != pin)
        at = &(*at)->next;
    if (*at)
        *at = pin->next;
    memset(pin, 0, sizeof(*pin));
}

void ua_cancel_pins(struct uaddr *u, const struct task *task)
{
    struct ua_pin **at = &u->pins;
    while (*at) {
        if ((*at)->task == task)
            *at = (*at)->next;
        else
            at = &(*at)->next;
    }
}

static int transfer(const struct uaddr *u, uint32_t va, void *buffer, uint32_t bytes, bool write)
{
    if (va < (u->win32 ? CIUKI_W32_LOW : CIUKI_IMAGE_BASE) || va >= CIUKI_MAIN_STACK_LIMIT ||
        bytes > CIUKI_MAIN_STACK_LIMIT - va)
        return -EFAULT;
    struct uaddr *owner=(struct uaddr *)u;
    struct ua_pin transaction={.base=va,.end=va+bytes,.mutation=true,.next=owner->pins};
    owner->pins=&transaction;
    unsigned pages=0; int result=0;
    uint8_t *b = buffer;
    while (bytes) {
        if (u->win32 && w32_view_access) { result=w32_view_access(owner,va,write); if(result)break; }
        uint32_t *p = pte((struct uaddr *)u, va, false);
        if (!p || !(*p & PTE_OWN)) { result=-EFAULT; break; }
        uint32_t off = va & (PAGE_SIZE - 1), n = PAGE_SIZE - off;
        if (n > bytes)
            n = bytes;
        uint8_t *physical = P2V((*p & ~(PAGE_SIZE - 1)) + off);
        if (write) memcpy(physical, b, n);
        else memcpy(b, physical, n);
        va += n;
        b += n;
        bytes -= n;
        /* 16 direct-map copies * 8 us = 128 us, with the whole range
         * reserved against unmap/protect until the supervisor copy ends. */
        if (!(++pages & 15u)) kpreempt_point();
    }
    ua_unpin(owner,&transaction);
    return result;
}

int ua_read(const struct uaddr *u, void *dst, uint32_t src, uint32_t bytes)
{
    return transfer(u, src, dst, bytes, false);
}

int ua_write(const struct uaddr *u, uint32_t dst, const void *src, uint32_t bytes)
{
    return transfer(u, dst, (void *)src, bytes, true);
}

static void zero_range(struct uaddr *u, uint32_t base, uint32_t end)
{
    while (base < end) {
        uint32_t *p = pte(u, base, false);
        uint32_t n = PAGE_SIZE - (base & (PAGE_SIZE - 1));
        if (n > end - base)
            n = end - base;
        memset(P2V((*p & ~(PAGE_SIZE - 1)) + (base & (PAGE_SIZE - 1))), 0, n);
        base += n;
    }
}

int32_t ua_brk(struct uaddr *u, uint32_t end)
{
    if (u->win32) return -EPERM;
    if (!end)
        return (int32_t)u->brk;
    if (end < u->heap_base || end >= CIUKI_HEAP_LIMIT)
        return -EINVAL;
    uint32_t old = u->brk, old_page = PAGE_ALIGN_UP(old), new_page = PAGE_ALIGN_UP(end);
    if (end == old)
        return (int32_t)old;
    if (pinned(u, end < old ? end : old, end > old ? end : old))
        return -EBUSY;
    if (end > old) {
        /* Existing partially exposed pages must remain writable. */
        if (old_page > old && !ua_range(u, old, old_page - old, PROT_WRITE))
            return -EBUSY;
        if (new_page > old_page) {
            int err = ua_map_at(u, old_page, new_page - old_page, PROT_READ | PROT_WRITE,
                                PROT_READ | PROT_WRITE, UA_HEAP, 0);
            if (err)
                return err == -EINVAL ? -ENOMEM : err;
        }
        /* Fresh pages were zeroed before publication. Only a previously
         * exposed tail can contain old heap bytes; it is < one page. */
        zero_range(u, old, end < old_page ? end : old_page);
    } else if (new_page < old_page) {
        struct ua_extent *l, *r;
        int err = prepare_split(u, new_page, old_page, true, &l, &r);
        if (err)
            return err;
        split(u, new_page, l);
        split(u, old_page, r);
        remove_range(u, new_page, old_page);
    }
    u->brk = end;
    return (int32_t)end;
}

void ua_release_owner(struct uaddr *u, uint32_t owner)
{
    if (!owner) return;
    for (;;) {
        struct ua_pin walk; walk_hold(u,&walk);
        struct ua_extent *e = u->head;
        while (e && e->owner != owner) { walk_step(); e = e->next; }
        walk_drop(u,&walk);
        if (!e) break;
        uint32_t base = e->base, end = e->end;
        if (pinned(u, base, end))
            panic("uaddr: releasing pinned thread mapping");
        remove_range(u, base, end);
    }
}

void ua_destroy(struct uaddr *u)
{
    if (!u->as.pd_phys)
        return;
    if (u->pins)
        panic("uaddr: destroy with live pins");
    if (read_cr3() == u->as.pd_phys)
        panic("uaddr: destroy active process");
    remove_range(u, u->win32 ? CIUKI_W32_LOW : CIUKI_IMAGE_BASE, CIUKI_MAIN_STACK_LIMIT);
    as_destroy(&u->as);
}

extern int copy_user(void *dst, const void *src, uint32_t bytes);
int copy_from_user(void *dst, uint32_t src, uint32_t bytes)
{
    struct process *p = proc_current();
    if (!p || !ua_range(p->memory, src, bytes, PROT_READ))
        return -EFAULT;
    return copy_user(dst, (const void *)(uintptr_t)src, bytes);
}

int copy_to_user(uint32_t dst, const void *src, uint32_t bytes)
{
    struct process *p = proc_current();
    if (!p || !ua_range(p->memory, dst, bytes, PROT_READ | PROT_WRITE))
        return -EFAULT;
    return copy_user((void *)(uintptr_t)dst, src, bytes);
}

/* Every mapped extent holds a ref; splitting preserves maximum rights and
 * backing identity. Failed page-table preparation rolls back the whole map. */
int32_t ua_map_shared(struct uaddr *u, uint32_t bytes,
                       uint32_t prot, uint32_t maximum, const struct ua_shared *shared)
{
    if (!bytes || (bytes & (PAGE_SIZE - 1)) || !shared ||
        !shared->object || !shared->retain || !shared->release || !shared->page ||
        (prot != PROT_READ && prot != (PROT_READ | PROT_WRITE)) ||
        (maximum != PROT_READ && maximum != (PROT_READ | PROT_WRITE))) return -EINVAL;
    if (prot & ~maximum) return -EACCES;
    uint32_t base = first_fit(u, bytes);
    if (!base) return -ENOMEM;
    int err = ua_map_at(u, base, bytes, PROT_NONE, maximum, UA_SHARED, 0);
    if (err) return err;
    struct ua_extent *e = ua_find(u, base);
    e->preparing = true;
    e->shared = *shared;
    e->shared.retain(e->shared.object);
    for (uint32_t off = 0; off < bytes; off += PAGE_SIZE) {
        uint32_t *p = pte(u, base + off, true);
        if (!p) { remove_range(u, base, base + bytes); return -ENOMEM; }
        *p = shared->page(shared->object, off / PAGE_SIZE) | PTE_OWN | PTE_SHARED;
        u->backing++;
        if(u->backing>u->peak_backing)u->peak_backing=u->backing;
        /* One mapping/table allocation per page, <=18 us; batch <=288 us. */
        if (!((off / PAGE_SIZE + 1) & 15u)) kpreempt_point();
    }
    protect_pages(u, base, base + bytes, prot);
    e->prot = prot; e->preparing = false;
    return (int32_t)base;
}

/* The Win32 layer owns allocation identity; this backend reuses the native
 * eager page accounting and rollback. No metadata/backing is published until
 * every allocation succeeds. Existing PTE_NEW bits are cleared at publication. */
static void w32_coalesce(struct uaddr *u)
{
    for (struct ua_extent *e = u->head; e && e->next;) {
        walk_step();
        struct ua_extent *n = e->next;
        if (e->kind == UA_W32 && n->kind == UA_W32 && e->end == n->base &&
            e->w32_allocation == n->w32_allocation && e->w32_state == n->w32_state &&
            e->w32_protect == n->w32_protect && e->owner == n->owner &&
            e->w32_locked == n->w32_locked && e->w32_type == n->w32_type &&
            e->generation == n->generation) {
            e->end = n->end; e->next = n->next; u->extents--; kfree(n);
        } else e = n;
    }
}

int ua_w32_change(struct uaddr *u, uint32_t base, uint32_t bytes,
                  uint32_t operation, uint32_t protect, uint32_t native, bool internal)
{
    uint32_t end;
    if (!u->win32 || !span(base, bytes, &end) || end > CIUKI_W32_SHARED + PAGE_SIZE)
        return -EINVAL;
    if (pinned(u, base, end)) return -EBUSY;
    struct ua_extent *first = ua_find(u, base);
    if (!first || first->kind != UA_W32) return -EEXIST;
    for (uint32_t va = base; va < end;) {
        struct ua_extent *e = ua_find(u, va);
        if (!e || e->kind != UA_W32 || e->w32_allocation != first->w32_allocation)
            return -EEXIST;
        if (e->w32_locked && !internal) return -EACCES;
        if (operation == W32_VM_PROTECT && e->w32_state != W32_MEM_COMMIT)
            return -EEXIST;
        va = e->end < end ? e->end : end;
    }
    struct ua_extent *l, *r;
    int err = prepare_split(u, base, end, false, &l, &r);
    if (err) return err;
    struct ua_pin transaction = { .base = 0, .end = CIUKI_MAIN_STACK_LIMIT,
        .mutation = true, .next = u->pins };
    u->pins = &transaction;
    if (operation == W32_VM_ALLOC) err = reserve_pages(u, base, end);
    if (err) { ua_unpin(u, &transaction); kfree(l); kfree(r); return err; }
    split(u, base, l); split(u, end, r);
    for (struct ua_extent *e = u->head; e; e = e->next) {
        walk_step();
        if (e->base < base || e->end > end) continue;
        if (operation == W32_VM_FREE) {
            ww_cancel(u->identity, e->generation, e->base, e->end);
            free_pages(u, e->base, e->end, false);
            e->w32_state = W32_MEM_RESERVE; e->w32_protect = 0; e->prot = 0;
        } else if (operation == W32_VM_PROTECT || e->w32_state != W32_MEM_COMMIT) {
            protect_pages(u, e->base, e->end, native);
            e->w32_state = W32_MEM_COMMIT; e->w32_protect = protect; e->prot = native;
        } else {
            /* Commit of an already committed page preserves its protection. */
            protect_pages(u, e->base, e->end, e->prot);
        }
    }
    w32_coalesce(u);
    ua_unpin(u, &transaction);
    return 0;
}

int ua_w32_release(struct uaddr *u, uint32_t base, bool internal)
{
    struct ua_extent *e = ua_find(u, base);
    if (!e || e->kind != UA_W32 || e->w32_allocation != base || e->base != base)
        return -EEXIST;
    struct ua_pin walk; walk_hold(u,&walk);
    uint32_t end = base;
    for (struct ua_extent *it = e; it && it->w32_allocation == base; it = it->next) {
        walk_step();
        if (!internal && (it->w32_locked || it->w32_type != W32_MEM_PRIVATE)) {
            walk_drop(u,&walk); return -EACCES;
        }
        end = it->end;
    }
    walk_drop(u,&walk);
    if (pinned(u, base, end)) return -EBUSY;
    remove_range(u, base, end);
    return 0;
}

/* f5-04 backend hook: section retains shared frames; private COW credits are
 * transferred exactly once to PTE_OWN. Native mappings never use this hook. */
int ua_w32_bind(struct uaddr *u,uint32_t va,uint32_t phys,uint32_t prot,bool shared)
{
    if(!u->win32 || (va&4095) || !phys || (phys&4095))return -EINVAL;
    uint32_t *p=pte(u,va,true);if(!p)return -ENOMEM;
    if((*p&PTE_OWN)&&!(*p&PTE_SHARED))return 0;
    if(!(*p&PTE_OWN))u->backing++;
    if(u->backing>u->peak_backing)u->peak_backing=u->backing;
    bool cow=shared&&!!(prot&0x80000000u);prot&=7;
    uint32_t value=phys|PTE_OWN|(shared?PTE_SHARED:0)|(cow?PTE_NEW:0);
    if(prot)value|=PTE_P|PTE_U|((prot&PROT_WRITE)&&!cow?PTE_W:0);
    set_pte(u,va,p,value);return 0;
}
