/* Transactional user VA extents and eagerly reserved private backing.
 * PTE_OWN retains a frame even when PROT_NONE clears Present. F0 page tables
 * never contain this bit and remain owned by core/vmm.c.
 * Research/decisions: build/f2-02/research.md; execution-abi.md, F2 memory.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/process.h>

#define PTE_OWN 0x200u
#define PTE_NEW 0x400u
static uint64_t next_generation;

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
    *p = value;
    flush(u, va);
}

static void free_pages(struct uaddr *u, uint32_t base, uint32_t end, bool only_new)
{
    for (uint32_t va = base; va < end; va += PAGE_SIZE) {
        uint32_t *p = pte(u, va, false);
        if (!p || (only_new && !(*p & PTE_NEW)))
            continue;
        if (*p & PTE_OWN) {
            pmm_free(*p & ~(PAGE_SIZE - 1));
            u->backing--;
        }
        set_pte(u, va, p, 0);
    }
    /* Empty page tables are reclaimed on rollback and on unmap, including
     * provisional tables created for a PROT_NONE reservation. */
    uint32_t *pd = P2V(u->as.pd_phys);
    for (uint32_t i = base >> 22; i <= (end - 1) >> 22; i++) {
        if (!(pd[i] & PTE_P))
            continue;
        uint32_t *pt = P2V(pd[i] & ~(PAGE_SIZE - 1));
        unsigned j = 0;
        while (j < 1024 && !pt[j])
            j++;
        if (j == 1024) {
            pmm_free(pd[i] & ~(PAGE_SIZE - 1));
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
        if (*p & PTE_OWN)
            continue;
        uint32_t phys = pmm_alloc();
        if (!phys)
            goto fail;
        memset(P2V(phys), 0, PAGE_SIZE);
        *p = phys | PTE_OWN | PTE_NEW;
        u->backing++;
    }
    return 0;
fail:
    free_pages(u, base, end, true);
    return -ENOMEM;
}

static void protect_pages(struct uaddr *u, uint32_t base, uint32_t end, uint32_t prot)
{
    for (uint32_t va = base; va < end; va += PAGE_SIZE) {
        uint32_t *p = pte(u, va, false);
        if (!p || !(*p & PTE_OWN))
            continue;
        uint32_t value = (*p & ~(PAGE_SIZE - 1)) | PTE_OWN;
        if (prot != PROT_NONE)
            value |= PTE_P | PTE_U | ((prot & PROT_WRITE) ? PTE_W : 0);
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
    for (struct ua_extent *e = u->head; e && e->base <= va; e = e->next)
        if (va < e->end)
            return e;
    return 0;
}

static bool arena(const struct ua_extent *e)
{
    return e->base >= CIUKI_MMAP_BASE && e->end <= CIUKI_MMAP_LIMIT;
}

static bool vacant(const struct uaddr *u, uint32_t base, uint32_t end)
{
    for (const struct ua_extent *e = u->head; e; e = e->next)
        if (base < e->end && end > e->base)
            return false;
    return true;
}

int ua_map_at(struct uaddr *u, uint32_t base, uint32_t bytes, uint32_t prot,
              uint32_t maximum, enum ua_kind kind, uint32_t owner)
{
    uint32_t end;
    int err = ua_protection(prot);
    if (err)
        return err;
    if (!span(base, bytes, &end) || base < CIUKI_IMAGE_BASE || !vacant(u, base, end))
        return -EINVAL;
    bool in_arena = base >= CIUKI_MMAP_BASE && end <= CIUKI_MMAP_LIMIT;
    if ((in_arena && u->arena_extents == CIUKI_MAPPING_MAX) || next_generation == UINT64_MAX)
        return -ENOMEM;
    struct ua_extent *e = kzalloc(sizeof(*e));
    if (!e)
        return -ENOMEM;
    if (prot && (err = reserve_pages(u, base, end))) {
        kfree(e);
        return err;
    }
    protect_pages(u, base, end, prot);
    *e = (struct ua_extent){ .base = base, .end = end, .prot = prot, .maximum = maximum,
        .kind = kind, .owner = owner, .generation = ++next_generation };
    struct ua_extent **at = &u->head;
    while (*at && (*at)->base < base)
        at = &(*at)->next;
    e->next = *at;
    *at = e;
    u->extents++;
    if (in_arena)
        u->arena_extents++;
    return 0;
}

static uint32_t first_fit(const struct uaddr *u, uint32_t bytes)
{
    uint32_t base = CIUKI_MMAP_BASE;
    for (const struct ua_extent *e = u->head; e; e = e->next) {
        if (e->end <= base)
            continue;
        if (e->base >= base && bytes <= e->base - base)
            break;
        base = e->end;
    }
    return base <= CIUKI_MMAP_LIMIT && bytes <= CIUKI_MMAP_LIMIT - base ? base : 0;
}

int32_t ua_mmap(struct uaddr *u, const struct ciuki_mmap_args *a)
{
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
    for (const struct ua_pin *p = u->pins; p; p = p->next)
        if (base < p->end && end > p->base)
            return true;
    return false;
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
    if (removing)
        for (struct ua_extent *e = u->head; e; e = e->next)
            if (arena(e) && e->base < end && e->end > base)
                final--;
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
    fresh->base = at;
    e->end = at;
    e->next = fresh;
    u->extents++;
    if (arena(fresh))
        u->arena_extents++;
}

static void remove_range(struct uaddr *u, uint32_t base, uint32_t end)
{
    struct ua_extent **at = &u->head;
    while (*at) {
        struct ua_extent *e = *at;
        if (e->base >= base && e->end <= end) {
            ww_cancel(u->identity, e->generation, e->base, e->end);
            free_pages(u, e->base, e->end, false);
            *at = e->next;
            u->extents--;
            if (arena(e))
                u->arena_extents--;
            kfree(e);
        } else {
            at = &e->next;
        }
    }
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
    for (struct ua_extent *e = u->head; e; e = e->next)
        if (base < e->end && end > e->base && protected_kind(e))
            return -EBUSY;
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
    int err = ua_protection(prot);
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
    if (prot && (err = reserve_pages(u, base, end))) {
        kfree(l);
        kfree(r);
        return err;
    }
    split(u, base, l);
    split(u, end, r);
    protect_pages(u, base, end, prot);
    for (struct ua_extent *e = u->head; e; e = e->next)
        if (e->base >= base && e->end <= end)
            e->prot = prot;
    return 0;
}

bool ua_range(const struct uaddr *u, uint32_t va, uint32_t bytes, uint32_t prot)
{
    if (!bytes)
        return true;
    if (va < CIUKI_IMAGE_BASE || va >= CIUKI_MAIN_STACK_LIMIT ||
        bytes > CIUKI_MAIN_STACK_LIMIT - va)
        return false;
    uint32_t end = va + bytes;
    while (va < end) {
        const struct ua_extent *e = ua_find(u, va);
        if (!e || e->kind == UA_GUARD || (e->prot & prot) != prot)
            return false;
        va = e->end < end ? e->end : end;
    }
    return true;
}

int ua_pin(struct uaddr *u, struct ua_pin *pin, uint32_t base, uint32_t bytes, bool write)
{
    if (!ua_range(u, base, bytes, PROT_READ | (write ? PROT_WRITE : 0)))
        return -EFAULT;
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
    if (va < CIUKI_IMAGE_BASE || va >= CIUKI_MAIN_STACK_LIMIT ||
        bytes > CIUKI_MAIN_STACK_LIMIT - va)
        return -EFAULT;
    uint8_t *b = buffer;
    while (bytes) {
        uint32_t *p = pte((struct uaddr *)u, va, false);
        if (!p || !(*p & PTE_OWN))
            return -EFAULT;
        uint32_t off = va & (PAGE_SIZE - 1), n = PAGE_SIZE - off;
        if (n > bytes)
            n = bytes;
        uint8_t *physical = P2V((*p & ~(PAGE_SIZE - 1)) + off);
        if (write) memcpy(physical, b, n);
        else memcpy(b, physical, n);
        va += n;
        b += n;
        bytes -= n;
    }
    return 0;
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
        zero_range(u, old, end);
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
    struct ua_extent *e = u->head;
    while (e) {
        uint32_t base = e->base, end = e->end;
        bool owned = e->owner == owner && owner;
        e = e->next;
        if (owned) {
            if (pinned(u, base, end))
                panic("uaddr: releasing pinned thread mapping");
            remove_range(u, base, end);
        }
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
    remove_range(u, CIUKI_IMAGE_BASE, CIUKI_MAIN_STACK_LIMIT);
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
