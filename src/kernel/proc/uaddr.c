/* Transactional user VA extents and object-backed demand paging.
 * PTE_OWN retains a frame even when PROT_NONE clears Present. F0 page tables
 * never contain this bit and remain owned by core/vmm.c.
 * Research/decisions: build/f2-02/research.md; execution-abi.md, F2 memory.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/process.h>
#include <ciuki/cpustat.h>
#include <ciuki/reclaim.h>

extern struct process *proc_current(void) __attribute__((weak));
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

/* Object metadata is resident; a PTE is only a revocable translation. Slots
 * exist at reservation, frames only at touch. Index links form the resident set. */
#define SLOT_COMMIT 1u
#define SLOT_NEW 2u
#define NO_SLOT UINT32_MAX
struct ua_frame { uint32_t phys, refs; bool zero; struct reclaim_page reclaim; };
struct ua_slot {
    struct ua_frame *frame;
    uint32_t previous, next, index;
    uint16_t flags, waiters;
};
#define OBJECT_BUCKETS 32u
#define OBJECT_SLOTS 64u
struct ua_slot_block {
    struct ua_slot_block *next;
    uint32_t first;
    struct ua_slot slots[OBJECT_SLOTS];
};
_Static_assert(sizeof(struct ua_slot_block)<=2040,"resident metadata fits small heap");
struct memory_object {
    uint32_t refs, count, resident, first;
    uint64_t generation;
    enum mm_backing_kind kind;
    struct ua_slot_block *blocks[OBJECT_BUCKETS];
    struct ua_extent *areas;
    /* C5 identity is retained by the external file adapter, never a path or
     * cluster number. No file page-in is inferred from a missing PTE. */
    struct vfs_file_identity *file;
    uint64_t file_generation, file_page;
    bool busy;
};
static struct ua_slot *object_slot(struct memory_object *o,uint32_t index)
{
    for (struct ua_slot_block *b=o->blocks[(index/OBJECT_SLOTS)%OBJECT_BUCKETS];b;b=b->next) {
        walk_step();
        if (index>=b->first && index-b->first<OBJECT_SLOTS) return &b->slots[index-b->first];
    }
    return 0;
}
static void object_free(struct memory_object *o)
{
    for (unsigned i=0;i<OBJECT_BUCKETS;i++) while (o->blocks[i]) {
        struct ua_slot_block *b=o->blocks[i]; o->blocks[i]=b->next;
        kfree(b); kpreempt_point();
    }
    kfree(o);
}
static struct ua_frame *zero_frame;
static uint32_t fault_epoch;
static struct mm_page_descriptor *frame_descriptor(struct ua_frame *f)
{ return pmm_descriptor ? pmm_descriptor(f->phys) : 0; }
static void frame_get(struct ua_frame *f)
{
    uint32_t flags=irq_save();
    f->refs++;
    struct mm_page_descriptor *d=frame_descriptor(f);
    if (d) d->refs=f->refs;
    irq_restore(flags);
}
static void frame_put(struct ua_frame *f)
{
    if (!f) return;
    uint32_t flags=irq_save();
    if (--f->refs) {
        struct mm_page_descriptor *d=frame_descriptor(f);
        if (d) d->refs=f->refs;
        irq_restore(flags);
        return;
    }
    if (reclaim_untrack) reclaim_untrack(&f->reclaim);
    if (f==zero_frame) zero_frame=0;
    irq_restore(flags);
    mm_free(f->phys,MM_USER_PRIVATE); kfree(f);
}
static struct ua_frame *frame_new(bool zero,uint32_t owner_id)
{
    uint32_t flags=irq_save();
    if (zero && zero_frame) {
        struct ua_frame *shared=zero_frame; frame_get(shared); irq_restore(flags); return shared;
    }
    irq_restore(flags);
    struct ua_frame *f=kmalloc(sizeof(*f));
    if (!f) return 0;
    uint32_t phys=pmm_alloc();
    if (!phys) { kfree(f); return 0; }
    mm_claim(phys,1,MM_USER_PRIVATE);
    *f=(struct ua_frame){.phys=phys,.refs=1,.zero=zero};
    struct mm_page_descriptor *d=frame_descriptor(f);
    if (d) { d->io_in_flight=1; d->backing_kind=zero?MM_BACKING_ZERO:MM_BACKING_PRIVATE; }
    memset(P2V(phys),0,PAGE_SIZE);
    if (d) d->io_in_flight=0;
    f->reclaim=(struct reclaim_page){.phys=phys,.generation=++fault_epoch,
        .owner=MM_USER_PRIVATE,.owner_id=owner_id};
    if (reclaim_track) reclaim_track(&f->reclaim);
    if (zero) zero_frame=f;
    return f;
}
static struct memory_object *object_new(uint32_t count)
{
    struct memory_object *o=kzalloc(sizeof(*o));
    if (!o) return 0;
    o->count=count; o->first=NO_SLOT; o->generation=next_generation;
    o->kind=MM_BACKING_PRIVATE;
    for (uint32_t first=0;first<count;first+=OBJECT_SLOTS) {
        struct ua_slot_block *b=kmalloc(sizeof(*b));
        if (!b) { object_free(o); return 0; }
        unsigned bucket=(first/OBJECT_SLOTS)%OBJECT_BUCKETS;
        b->first=first; b->next=o->blocks[bucket]; o->blocks[bucket]=b;
        for (unsigned i=0;i<OBJECT_SLOTS;i++) {
            b->slots[i]=(struct ua_slot){.index=first+i,.previous=NO_SLOT,.next=NO_SLOT};
            if (!((i+1)&15u)) kpreempt_point();
        }
    }
    return o;
}
static void object_attach(struct ua_extent *e,struct memory_object *o)
{
    e->backing=o; e->object_next=o->areas; o->areas=e; o->refs++;
}
static void slot_link(struct memory_object *o,uint32_t index,struct ua_frame *f)
{
    struct ua_slot *s=object_slot(o,index);
    s->frame=f; s->previous=NO_SLOT; s->next=o->first;
    if (o->first!=NO_SLOT) object_slot(o,o->first)->previous=index;
    o->first=index; o->resident++;
}
static void slot_clear(struct memory_object *o,uint32_t index)
{
    struct ua_slot *s=object_slot(o,index);
    if (s->frame) {
        if (s->previous!=NO_SLOT) object_slot(o,s->previous)->next=s->next;
        else o->first=s->next;
        if (s->next!=NO_SLOT) object_slot(o,s->next)->previous=s->previous;
        frame_put(s->frame); o->resident--;
    }
    if ((s->flags&SLOT_COMMIT) && ledger_promise) ledger_promise(MM_USER_PRIVATE,0,-1);
    *s=(struct ua_slot){.index=index,.previous=NO_SLOT,.next=NO_SLOT};
}
/* The removing area's VA is excluded from use by its mutation pin. Other
 * aliases retain their slot. No raw predecessor survives an unprotected yield. */
static void object_detach(struct ua_extent *e)
{
    struct memory_object *o=e->backing;
    if (!o) return;
    bool was_busy=o->busy; o->busy=true;
    struct ua_extent **at=&o->areas;
    while (*at!=e) { walk_step(); at=&(*at)->object_next; }
    *at=e->object_next;
    for (uint32_t i=e->page_index;i<e->page_index+(e->end-e->base)/PAGE_SIZE;i++) {
        bool used=false;
        for (struct ua_extent *a=o->areas;a;a=a->object_next) {
            walk_step();
            if (i>=a->page_index && i-a->page_index<(a->end-a->base)/PAGE_SIZE) { used=true; break; }
        }
        if (!used) slot_clear(o,i);
        if (!((i-e->page_index+1)&15u)) kpreempt_point();
    }
    e->backing=0; e->object_next=0;
    if (!--o->refs) { object_free(o); }
    else o->busy=was_busy;
}
static struct ua_slot *area_slot(struct ua_extent *e,uint32_t va)
{ return object_slot(e->backing,e->page_index+(va-e->base)/PAGE_SIZE); }
static bool object_area(const struct ua_extent *e)
{ return e && e->backing && e->kind!=UA_SHARED &&
         (e->kind!=UA_W32 || !e->w32_type || e->w32_type==W32_MEM_PRIVATE); }

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
    if ((*p&PTE_OWN) && pmm_descriptor) {
        struct mm_page_descriptor *d=pmm_descriptor(*p&~4095u);
        if (d && (*p&0x60u)) {
            uint32_t epoch=++fault_epoch;
            if (*p&0x20u) d->accessed_epoch=epoch;
            if (*p&0x40u) d->dirty_epoch=d->dirty_epoch==UINT32_MAX?1:d->dirty_epoch+1;
        }
    }
    if ((*p&~4095u)==(value&~4095u)) value|=*p&0x60u;
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
        struct ua_extent *e=ua_find(u,va);
        if (object_area(e)) {
            struct ua_slot *slot=area_slot(e,va);
            if (only_new) {
                if (slot->flags&SLOT_NEW) slot_clear(e->backing,slot->index);
                continue;
            }
            uint32_t *entry=pte(u,va,false);
            if (entry && (*entry&PTE_OWN)) { u->backing--; set_pte(u,va,entry,0); }
            continue;
        }
        uint32_t *p = pte(u, va, false);
        if (!p || (only_new && (!(*p & PTE_NEW) || (*p & PTE_SHARED))))
            continue;
        uint32_t old=*p;
        /* Revoke and flush eager/COW translations before PMM can publish
         * their frame as free, just as for object-backed aliases above. */
        set_pte(u, va, p, 0);
        if (old & PTE_OWN) {
            if (!(old & PTE_SHARED))
                mm_free(old & ~(PAGE_SIZE - 1), MM_USER_PRIVATE);
            if (!(old & PTE_SHARED) && ledger_promise) ledger_promise(MM_USER_PRIVATE,0,-1);
            u->backing--;
        }
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
        struct ua_extent *e=ua_find(u,va);
        if (object_area(e)) {
            struct ua_slot *slot=area_slot(e,va);
            if (!(slot->flags&SLOT_COMMIT)) {
                if (ledger_commit) { if (!ledger_commit(MM_USER_PRIVATE,1)) goto fail; }
                else if (ledger_promise) ledger_promise(MM_USER_PRIVATE,0,1);
                slot->flags|=SLOT_COMMIT|SLOT_NEW;
            }
            if (!(((va-base)/PAGE_SIZE+1)&15u)) kpreempt_point();
            continue;
        }
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
        struct ua_extent *e=ua_find(u,va);
        if (object_area(e)) area_slot(e,va)->flags&=~SLOT_NEW;
        uint32_t *p = pte(u, va, false);
        if (!p || !(*p & PTE_OWN))
            continue;
        /* Shared+NEW denotes a stable COW alias; NEW alone is provisional
         * private backing. Never expose shared COW pages as writable, even
         * during a yielding VirtualProtect transaction. */
        bool cow=object_area(e) ? (area_slot(e,va)->frame &&
                 (area_slot(e,va)->frame->zero || area_slot(e,va)->frame->refs>1)) :
                 ((*p&PTE_SHARED)&&(*p&PTE_NEW));
        uint32_t value = (*p & ~(PAGE_SIZE - 1)) | PTE_OWN |
            (object_area(e)?(cow?PTE_SHARED|PTE_NEW:0):((*p&PTE_SHARED)|(cow?PTE_NEW:0)));
        if (prot != PROT_NONE)
            value |= PTE_P | PTE_U | ((prot & PROT_WRITE) && !cow ? PTE_W : 0);
        set_pte(u, va, p, value);
    }
}

int ua_init(struct uaddr *u, void *identity)
{
    memset(u, 0, sizeof(*u));
    u->identity = identity;
    int err=as_create(&u->as);
    if (!err) u->as.memory=u;
    return err;
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

static int map_area(struct uaddr *u, uint32_t base, uint32_t bytes, uint32_t prot,
              uint32_t maximum, enum ua_kind kind, uint32_t owner, bool pending)
{
    uint32_t end;
    if (u->closing) return -EFAULT;
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
    if (kind!=UA_SHARED && kind!=UA_GUARD) {
        e->space=u;
        struct memory_object *o=object_new((end-base)/PAGE_SIZE);
        if (o) object_attach(e,o); else err=-ENOMEM;
    }
    if (!err && prot) err=reserve_pages(u,base,end);
    if (err) {
        /* Other disjoint mappings may have changed the list while yielded. */
        walk_hold(u,&walk);
        at = &u->head;
        while (*at != e) { walk_step(); at = &(*at)->next; }
        *at = e->next; u->extents--;
        if (in_arena) u->arena_extents--;
        object_detach(e); kfree(e);
        walk_drop(u,&walk);
        return err;
    }
    protect_pages(u, base, end, prot);
    if (ledger_promise) ledger_promise(kind==UA_SHARED ? MM_USER_SHARED : MM_USER_PRIVATE,(end-base)/PAGE_SIZE,0);
    e->prot = prot; e->preparing = pending;
    if (cpu_path_add && cpu_path_clock) cpu_path_add(CPU_ZERO, cpu_path_clock() - start);
    return 0;
}

int ua_map_at(struct uaddr *u, uint32_t base, uint32_t bytes, uint32_t prot,
              uint32_t maximum, enum ua_kind kind, uint32_t owner)
{ return map_area(u,base,bytes,prot,maximum,kind,owner,false); }

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
        if ((e->preparing || (e->backing && e->backing->busy)) && base < e->end && end > e->base) { busy=true; break; }
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
    fresh->page_index+=(at-e->base)/PAGE_SIZE;
    fresh->base = at;
    if (fresh->backing) object_attach(fresh,fresh->backing);
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
            object_detach(e);
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

/* All callers retain a range pin before a possible service/sleep. UP object
 * ownership coalesces even faults through different address-space aliases. */
static bool fault_cancelled(struct uaddr *u)
{
    struct process *p=proc_current?proc_current():0;
    return u->closing || (p && p->state==PROC_STOPPING);
}
static int resolve_page(struct uaddr *u,uint32_t va,bool write,bool supervisor)
{
    struct ua_extent *e=ua_find(u,va);
    if (fault_cancelled(u)) return u->closing?-EFAULT:-EINTR;
    if (!e || e->preparing || e->kind==UA_GUARD) return -EFAULT;
    if (!supervisor && (!(e->prot&PROT_READ) || (write && !(e->prot&PROT_WRITE)))) return -EFAULT;
    uint32_t *bound=pte(u,va,false);
    bool section_fault=!bound || !(*bound&PTE_OWN) ||
        (write && (*bound&(PTE_SHARED|PTE_NEW))==(PTE_SHARED|PTE_NEW));
    if (section_fault && g_current && g_current->preempt_holds) return -EBUSY;
    if (u->win32 && w32_view_access && section_fault) {
        int err=w32_view_access(u,va,write);
        if (err) return err;
    }
    if (!object_area(e)) {
        uint32_t *p=pte(u,va,false);
        return p && (*p&PTE_OWN) ? 0 : -EFAULT;
    }
    struct memory_object *o=e->backing;
    if (o->kind!=MM_BACKING_PRIVATE && o->kind!=MM_BACKING_SHARED && o->kind!=MM_BACKING_ZERO)
        return -EFAULT; /* file/pagefile/device adapters must supply verified backing */
    struct ua_slot *s=area_slot(e,va);
    if (!(s->flags&SLOT_COMMIT) || (e->kind==UA_W32 && e->w32_state!=W32_MEM_COMMIT)) return -EFAULT;
    uint64_t generation=e->generation, object_generation=o->generation;
    while (o->busy) {
        if (reclaim_fault) reclaim_fault(true);
        s->waiters++;
        struct ua_frame *waiting=s->frame;
        if (waiting) {
            frame_get(waiting);
            struct mm_page_descriptor *d=frame_descriptor(waiting);
            if (d) d->waiters++;
        }
        task_sleep_ms(1);
        if (waiting) {
            struct mm_page_descriptor *d=frame_descriptor(waiting);
            if (d) d->waiters--;
            frame_put(waiting);
        }
        s->waiters--;
        /* The pin keeps metadata alive; every wake revalidates identity. */
        if (fault_cancelled(u)) return u->closing?-EFAULT:-EINTR;
        if (e->generation!=generation || o->generation!=object_generation) return -EFAULT;
    }
    o->busy=true;
    bool needs=!s->frame || (write && (s->frame->zero || s->frame->refs>1));
    if (needs && ledger_assert_pinned_context) ledger_assert_pinned_context(true,(uintptr_t)__builtin_return_address(0));
    if (needs && g_current && g_current->preempt_holds) { o->busy=false; return -EBUSY; }
    struct ua_frame *fresh=0, *retired=0;
    int err=0;
    if (needs) {
        /* Ordinary task context, before any pager/filesystem lock. Retain
         * operation ownership across the service and recheck generation. */
        kpreempt_point();
        if (fault_cancelled(u)) { err=u->closing?-EFAULT:-EINTR; goto done; }
        if (e->generation!=generation || o->generation!=object_generation) { err=-EFAULT; goto done; }
        fresh=frame_new(!write,u->as.pd_phys);
        if (!fresh) { err=-ENOMEM; goto done; }
        if (write && s->frame && !s->frame->zero) {
            struct mm_page_descriptor *d=frame_descriptor(fresh);
            if (d) d->io_in_flight=1;
            memcpy(P2V(fresh->phys),P2V(s->frame->phys),PAGE_SIZE);
            if (d) d->io_in_flight=0;
        }
    }
    uint32_t page=PAGE_ALIGN_DOWN(va), *entry=pte(u,page,true);
    if (!entry) { err=-ENOMEM; frame_put(fresh); goto done; }
    if (fresh) {
        if (s->frame) { retired=s->frame; s->frame=fresh; }
        else slot_link(o,s->index,fresh);
    }
    struct ua_frame *f=s->frame;
    struct mm_page_descriptor *d=frame_descriptor(f);
    if (d) {
        d->accessed_epoch=++fault_epoch;
        d->reserved|=MM_PAGE_TOUCHED;
        if (ledger_page_activity) ledger_page_activity(f->phys,true);
        if (write) d->dirty_epoch=d->dirty_epoch==UINT32_MAX?1:d->dirty_epoch+1;
        d->backing_slot=s->index;
        d->backing_kind=f->zero?MM_BACKING_ZERO:o->kind;
    }
    /* Replace translations in all already-resident shared aliases. Publishing
     * one new frame may never leave a second alias on the old zero/COW PFN. */
    for (struct ua_extent *a=o->areas;a;a=a->object_next) {
        walk_step();
        uint32_t index=s->index;
        if (index<a->page_index || index-a->page_index>=(a->end-a->base)/PAGE_SIZE) continue;
        uint32_t address=a->base+(index-a->page_index)*PAGE_SIZE;
        uint32_t *p=pte(a->space,address,false);
        if (!p || (!(*p&PTE_OWN) && (a!=e || address!=page))) continue;
        if (!(*p&PTE_OWN)) a->space->backing++;
        bool cow=f->zero || f->refs>1;
        uint32_t value=f->phys|PTE_OWN|(cow?PTE_SHARED|PTE_NEW:0);
        if (a->prot) value|=PTE_P|PTE_U|((a->prot&PROT_WRITE)&&!cow?PTE_W:0);
        set_pte(a->space,address,p,value);
    }
done:
    /* Retain the old PFN until the final stale alias has been invalidated,
     * including across every service point in the alias walk. */
    frame_put(retired);
    o->busy=false;
    return err;
}
int ua_fault(struct uaddr *u,uint32_t address,uint32_t error)
{
    if (reclaim_fault) reclaim_fault(false);
    if (!u || !(error&4) || (error&8) || address>=CIUKI_MAIN_STACK_LIMIT || fault_cancelled(u)) return -EFAULT;
    uint32_t wanted=(error&16)?PROT_EXEC:PROT_READ|((error&2)?PROT_WRITE:0);
    if (!ua_range(u,address,1,wanted)) return -EFAULT; /* unpublished/guard/invalid stays a fault */
    struct ua_pin pin={.base=PAGE_ALIGN_DOWN(address),.end=PAGE_ALIGN_DOWN(address)+PAGE_SIZE,
                      .task=g_current,.next=u->pins};
    u->pins=&pin;
    int err=-EFAULT;
    for (;;) {
        bool changing=false;
        for (struct ua_pin *p=u->pins;p;p=p->next)
            if (p->mutation && address>=p->base && address<p->end) { changing=true; break; }
        if (!changing) break;
        if (g_current && g_current->preempt_holds) { err=-EBUSY; goto done; }
        task_sleep_ms(1); /* no area pointer retained; validate the new area below */
        if (fault_cancelled(u)) goto done;
    }
    if (!ua_range(u,address,1,wanted)) goto done;
    struct ua_extent *e=ua_find(u,address);
    if (error&1) {
        if (!(error&2) || !object_area(e)) goto done;
        struct ua_frame *f=area_slot(e,address)->frame;
        uint32_t *p=pte(u,address,false);
        if (!f || !p || (*p&PTE_W) || (*p&(PTE_SHARED|PTE_NEW))!=(PTE_SHARED|PTE_NEW)) goto done;
    }
    err=resolve_page(u,address,!!(error&2),false);
done:
    ua_unpin(u,&pin); return err;
}

void ua_backing_identity(struct uaddr *u,uint32_t base,uint32_t bytes,
                          struct vfs_file_identity *file,uint64_t generation,uint64_t page)
{
    struct ua_pin walk; walk_hold(u,&walk);
    for (struct ua_extent *e=u->head;e;e=e->next) {
        walk_step();
        if (e->base<base || e->base>=base+bytes || !e->backing) continue;
        e->backing->kind=file?MM_BACKING_FILE:MM_BACKING_SHARED;
        e->backing->file=file; e->backing->file_generation=generation;
        e->backing->file_page=page;
    }
    walk_drop(u,&walk);
}
int32_t ua_alias(struct uaddr *dst,struct uaddr *src,uint32_t base,uint32_t bytes,uint32_t prot,bool private_copy)
{
    uint32_t end;
    if (!span(base,bytes,&end) || end-base!=bytes || dst->win32 || src->win32 || ua_protection(prot)) return -EINVAL;
    if (pinned(src,base,end)) return -EBUSY;
    struct ua_extent *source=ua_find(src,base);
    if (!object_area(source) || end>source->end || (prot&~source->maximum)) return -EACCES;
    struct memory_object *old=source->backing;
    if (private_copy && old->kind!=MM_BACKING_PRIVATE) return -EOPNOTSUPP;
    struct ua_pin pin={.base=base,.end=end,.mutation=true,.task=g_current,.next=src->pins};
    src->pins=&pin; old->busy=true;
    uint32_t address=0; int err=0;
    for (uint32_t off=0;off<bytes;off+=PAGE_SIZE) {
        if (!(area_slot(source,base+off)->flags&SLOT_COMMIT)) { err=-EFAULT; break; }
        if (!((off/PAGE_SIZE+1)&15u)) kpreempt_point();
    }
    if (!err) {
        address=first_fit(dst,bytes);
        err=address?map_area(dst,address,bytes,prot,source->maximum,UA_ANON,0,true):-ENOMEM;
    }
    if (!err && !prot) {
        err=reserve_pages(dst,address,address+bytes);
        if (!err) protect_pages(dst,address,address+bytes,0);
        else remove_range(dst,address,address+bytes);
    }
    if (!err) {
        struct ua_extent *e=ua_find(dst,address);
        uint32_t start=source->page_index+(base-source->base)/PAGE_SIZE;
        if (private_copy) {
            for (uint32_t i=0;i<bytes/PAGE_SIZE;i++) {
                struct ua_frame *f=object_slot(old,start+i)->frame;
                if (f) {
                    uint32_t *original=pte(src,base+i*PAGE_SIZE,false);
                    if (original && (*original&PTE_OWN))
                        set_pte(src,base+i*PAGE_SIZE,original,(*original&~PTE_W)|PTE_SHARED|PTE_NEW);
                    /* Revoke writable source translations before a service
                     * can expose a newly shared PFN to either private area. */
                    frame_get(f); slot_link(e->backing,i,f);
                }
                if (!((i+1)&15u)) kpreempt_point();
            }
            protect_pages(src,base,end,source->prot);
        } else {
            object_detach(e); e->page_index=start;
            old->kind=MM_BACKING_SHARED; object_attach(e,old);
        }
        e->preparing=false;
    }
    old->busy=false; ua_unpin(src,&pin);
    return err?err:(int32_t)address;
}

int ua_pin(struct uaddr *u, struct ua_pin *pin, uint32_t base, uint32_t bytes, bool write)
{
    if (!ua_range(u, base, bytes, PROT_READ | (write ? PROT_WRITE : 0)))
        return -EFAULT;
    for (struct ua_pin *p = u->pins; p; p = p->next)
        if (p->mutation && base < p->end && base + bytes > p->base) return -EBUSY;
    *pin = (struct ua_pin){ .base = base, .end = base + bytes, .prepared_end=PAGE_ALIGN_DOWN(base),
        .task = g_current, .next = u->pins };
    u->pins = pin;
    for (uint32_t va=PAGE_ALIGN_DOWN(base); bytes && va<(uint64_t)base+bytes; va+=PAGE_SIZE) {
        int err=resolve_page(u,va,write,false);
        struct ua_extent *e=ua_find(u,va);
        if (!err && object_area(e)) {
            struct ua_frame *f=area_slot(e,va)->frame;
            if (f && (f->zero || f->refs>1)) err=resolve_page(u,va,true,true);
        }
        if (err) { ua_unpin(u,pin); return err; }
        uint32_t *entry=pte(u,va,false);
        if (!err && u->win32 && !write && e && (e->prot&PROT_WRITE) &&
            entry && (*entry&(PTE_SHARED|PTE_NEW))==(PTE_SHARED|PTE_NEW) && w32_view_access) {
            err=w32_view_access(u,va,true);
            if (err) { ua_unpin(u,pin); return err; }
        }
        struct mm_page_descriptor *d=pmm_descriptor?pmm_descriptor(*entry&~4095u):0;
        if (d) d->pins++;
        pin->prepared_end=va+PAGE_SIZE;
        if (!(((va-PAGE_ALIGN_DOWN(base))/PAGE_SIZE+1)&15u)) kpreempt_point();
    }
    return 0;
}

void ua_unpin(struct uaddr *u, struct ua_pin *pin)
{
    struct ua_pin **at=&u->pins;
    while (*at && *at!=pin) at=&(*at)->next;
    if (*at) {
        /* Keep the VA lease linked until every PFN debit is retired. A
         * service point must not let unmap free the remaining pinned pages. */
        unsigned work=0;
        while (!pin->mutation && pin->prepared_end>PAGE_ALIGN_DOWN(pin->base)) {
            pin->prepared_end-=PAGE_SIZE;
            uint32_t *entry=pte(u,pin->prepared_end,false);
            struct mm_page_descriptor *d=entry && (*entry&PTE_OWN) && pmm_descriptor?pmm_descriptor(*entry&~4095u):0;
            if (d && d->pins) d->pins--;
            if (!(++work&15u)) kpreempt_point();
        }
        /* Other stack-owned pins can change predecessors during service. */
        at=&u->pins;
        while (*at && *at!=pin) at=&(*at)->next;
        if (*at) *at=pin->next;
    }
    memset(pin,0,sizeof(*pin));
}

void ua_cancel_pins(struct uaddr *u, const struct task *task)
{
    struct ua_pin **at = &u->pins;
    while (*at) {
        if ((*at)->task == task) {
            struct ua_pin *pin=*at;
            ua_unpin(u,pin); at=&u->pins;
        }
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
        result=resolve_page(owner,va,write,true); if(result)break;
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

static int zero_range(struct uaddr *u, uint32_t base, uint32_t end)
{
    while (base < end) {
        struct ua_extent *e=ua_find(u,base);
        if (!object_area(e) || !area_slot(e,base)->frame) return 0; /* untouched tail is zero */
        int err=resolve_page(u,base,true,true); if (err) return err;
        uint32_t *p = pte(u, base, false);
        uint32_t n = PAGE_SIZE - (base & (PAGE_SIZE - 1));
        if (n > end - base)
            n = end - base;
        memset(P2V((*p & ~(PAGE_SIZE - 1)) + (base & (PAGE_SIZE - 1))), 0, n);
        base += n;
    }
    return 0;
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
        int err=zero_range(u, old, end < old_page ? end : old_page);
        if (err) { if (new_page>old_page) remove_range(u,old_page,new_page); return err; }
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
    u->closing=true;
    for (;;) {
        bool busy=false;
        for (struct ua_extent *e=u->head;e;e=e->next) {
            walk_step(); if (e->backing && e->backing->busy) { busy=true; break; }
        }
        if (!busy) break;
        task_sleep_ms(1); /* rewalk; no area pointer survives the sleep */
    }
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
    struct ua_pin pin;
    int err=ua_pin(p->memory,&pin,src,bytes,false);
    if (err) return err;
    err=copy_user(dst,(const void *)(uintptr_t)src,bytes);
    ua_unpin(p->memory,&pin); return err;
}

int copy_to_user(uint32_t dst, const void *src, uint32_t bytes)
{
    struct process *p = proc_current();
    if (!p || !ua_range(p->memory, dst, bytes, PROT_READ | PROT_WRITE))
        return -EFAULT;
    struct ua_pin pin;
    int err=ua_pin(p->memory,&pin,dst,bytes,true);
    if (err) return err;
    err=copy_user((void *)(uintptr_t)dst,src,bytes);
    ua_unpin(p->memory,&pin); return err;
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
            e->generation == n->generation && e->backing==n->backing &&
            e->page_index+(e->end-e->base)/PAGE_SIZE==n->page_index) {
            e->end = n->end; e->next = n->next;
            object_detach(n); u->extents--; kfree(n);
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
            if (object_area(e)) for (uint32_t va=e->base;va<e->end;va+=PAGE_SIZE) {
                slot_clear(e->backing,e->page_index+(va-e->base)/PAGE_SIZE);
                if (!(((va-e->base)/PAGE_SIZE+1)&15u)) kpreempt_point();
            }
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
