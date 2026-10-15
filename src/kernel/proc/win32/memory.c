/* Windows reservation/commit semantics over uaddr's eager backing.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/win32.h>

uint32_t w32_protection(uint32_t protect, uint32_t *native)
{
    uint32_t p = protect & ~(W32_PAGE_GUARD | W32_PAGE_NOCACHE), n;
    switch (p) {
    case W32_PAGE_NOACCESS: n = PROT_NONE; break;
    case W32_PAGE_READONLY: n = PROT_READ; break;
    case W32_PAGE_READWRITE: case W32_PAGE_WRITECOPY: n = PROT_READ | PROT_WRITE; break;
    case W32_PAGE_EXECUTE: case W32_PAGE_EXECUTE_READ: n = PROT_READ | PROT_EXEC; break;
    case W32_PAGE_EXECUTE_READWRITE: case W32_PAGE_EXECUTE_WRITECOPY:
        n = PROT_READ | PROT_WRITE | PROT_EXEC; break;
    default: return W32_STATUS_INVALID_PARAMETER;
    }
    if (p == W32_PAGE_NOACCESS && (protect & W32_PAGE_GUARD)) return W32_STATUS_INVALID_PARAMETER;
    *native = protect & W32_PAGE_GUARD ? PROT_NONE : n;
    return 0;
}

static bool round_span(uint32_t address, uint32_t bytes, uint32_t alignment,
                       uint32_t limit, uint32_t *base, uint32_t *end)
{
    if (!bytes || address >= limit || bytes > limit - address) return false;
    *base = address & ~(alignment - 1);
    *end = PAGE_ALIGN_UP(address + bytes);
    return *end <= limit && *end > *base;
}

static uint32_t placement(const struct uaddr *u, uint32_t bytes, uint32_t limit, bool top)
{
    uint32_t next = CIUKI_W32_LOW, result = 0;
    for (const struct ua_extent *e = u->head;; e = e ? e->next : 0) {
        uint32_t end = e && e->base < limit ? e->base : limit;
        if (next < end && bytes <= end - next) {
            if (!top) return next;
            result = (end - bytes) & ~(CIUKI_W32_GRANULARITY - 1);
        }
        if (!e || e->end >= limit) break;
        if (e->end > next) next = (e->end + CIUKI_W32_GRANULARITY - 1) & ~(CIUKI_W32_GRANULARITY - 1);
    }
    return result;
}

static int reserve(struct uaddr *u, uint32_t base, uint32_t bytes, uint32_t protect, uint32_t owner)
{
    for (const struct ua_extent *e = u->head; e; e = e->next)
        if (base < e->end && base + bytes > e->base) return -EEXIST;
    int err = ua_map_at(u, base, bytes, 0, PROT_READ | PROT_WRITE | PROT_EXEC, UA_W32, owner);
    if (err) return err;
    struct ua_extent *e = ua_find(u, base);
    e->w32_allocation = base; e->w32_allocation_protect = protect;
    e->w32_state = W32_MEM_RESERVE; e->w32_type = W32_MEM_PRIVATE;
    return 0;
}

uint32_t w32_vm_alloc(struct uaddr *u, struct ciuki_w32_vm *a)
{
    uint32_t known = W32_MEM_RESERVE | W32_MEM_COMMIT | W32_MEM_TOP_DOWN;
    uint32_t native, status = w32_protection(a->protect, &native), base, end;
    if (status) return status;
    if (!u->win32 || !a->flags || a->flags & ~known ||
        !(a->flags & (W32_MEM_RESERVE | W32_MEM_COMMIT)) ||
        ((a->flags & W32_MEM_TOP_DOWN) && a->address)) return W32_STATUS_INVALID_PARAMETER;
    bool reserving = !!(a->flags & W32_MEM_RESERVE) || !a->address;
    if (!round_span(a->address, a->bytes, reserving ? CIUKI_W32_GRANULARITY : PAGE_SIZE,
                    CIUKI_W32_APP_LIMIT, &base, &end)) return W32_STATUS_INVALID_PARAMETER;
    uint32_t bytes = end - base;
    if (!a->address) {
        base = placement(u, bytes, CIUKI_W32_APP_LIMIT, !!(a->flags & W32_MEM_TOP_DOWN));
        if (!base) return W32_STATUS_NO_MEMORY;
    } else if (base < CIUKI_W32_LOW) return W32_STATUS_CONFLICTING_ADDRESSES;
    int err = reserving ? reserve(u, base, bytes, a->protect, 0) : 0;
    bool fresh = reserving && !err;
    if (!err && (a->flags & W32_MEM_COMMIT))
        err = ua_w32_change(u, base, bytes, W32_VM_ALLOC, a->protect, native, false);
    if (err) {
        if (fresh) ua_w32_release(u, base, true);
        return w32_status(err);
    }
    a->address = base; a->bytes = bytes;
    return 0;
}

uint32_t w32_vm_free(struct uaddr *u, struct ciuki_w32_vm *a)
{
    if (a->flags == W32_MEM_RELEASE) {
        if (a->bytes) return W32_STATUS_INVALID_PARAMETER;
        return w32_status(ua_w32_release(u, a->address, false));
    }
    if (a->flags != W32_MEM_DECOMMIT) return W32_STATUS_INVALID_PARAMETER;
    struct ua_extent *e = ua_find(u, a->address);
    if (!e || e->kind != UA_W32) return W32_STATUS_CONFLICTING_ADDRESSES;
    uint32_t bytes = a->bytes, base, end;
    if (!bytes) {
        if (a->address != e->w32_allocation) return W32_STATUS_INVALID_PARAMETER;
        uint32_t last = e->end;
        for (struct ua_extent *n = e->next; n && n->w32_allocation == e->w32_allocation; n = n->next)
            last = n->end;
        bytes = last - a->address;
    }
    if (!round_span(a->address, bytes, PAGE_SIZE, CIUKI_W32_APP_LIMIT, &base, &end))
        return W32_STATUS_INVALID_PARAMETER;
    for (struct ua_extent *n = e; n && n->base < end; n = n->next)
        if (n->w32_type != W32_MEM_PRIVATE) return W32_STATUS_ACCESS_DENIED;
    return w32_status(ua_w32_change(u, base, end - base, W32_VM_FREE, 0, 0, false));
}

uint32_t w32_vm_protect(struct uaddr *u, struct ciuki_w32_vm *a)
{
    uint32_t base, end, native, status = w32_protection(a->protect, &native);
    if (status) return status;
    if (a->flags || !round_span(a->address, a->bytes, PAGE_SIZE, CIUKI_W32_BUILTIN_LIMIT, &base, &end))
        return W32_STATUS_INVALID_PARAMETER;
    const struct ua_extent *e = ua_find(u, base);
    uint32_t old = e ? e->w32_protect : 0;
    int err = ua_w32_change(u, base, end - base, W32_VM_PROTECT, a->protect, native, false);
    if (err) return w32_status(err);
    a->old_protect = old; a->address = base; a->bytes = end - base;
    return 0;
}

static bool same_region(const struct ua_extent *a, const struct ua_extent *b)
{
    return a->w32_allocation == b->w32_allocation && a->w32_state == b->w32_state &&
        a->w32_protect == b->w32_protect && a->w32_type == b->w32_type;
}
uint32_t w32_vm_query(const struct uaddr *u, uint32_t address, struct ciuki_w32_memory_info *out)
{
    if (!u->win32 || address >= CIUKI_W32_LIMIT) return W32_STATUS_INVALID_PARAMETER;
    uint32_t base = address & ~(PAGE_SIZE - 1);
    const struct ua_extent *e = ua_find(u, base);
    memset(out, 0, sizeof(*out)); out->base = base;
    if (!e) {
        uint32_t end = CIUKI_W32_LIMIT;
        for (const struct ua_extent *n = u->head; n; n = n->next)
            if (n->base > base) { end = n->base; break; }
        /* Unavailable address holes are reported reserved/no-access. */
        if (base < CIUKI_W32_LOW) end = CIUKI_W32_LOW;
        else if (base >= CIUKI_W32_SHARED + PAGE_SIZE) end = CIUKI_W32_LIMIT;
        out->state = base < CIUKI_W32_LOW || base >= CIUKI_W32_SHARED + PAGE_SIZE ? W32_MEM_RESERVE : W32_MEM_FREE;
        out->protect = out->state == W32_MEM_RESERVE ? W32_PAGE_NOACCESS : 0;
        out->region_size = end - base;
        return 0;
    }
    uint32_t end = e->end;
    for (const struct ua_extent *n = e->next; n && n->base == end && same_region(e, n); n = n->next) end = n->end;
    out->allocation_base = e->kind == UA_W32 ? e->w32_allocation : e->base;
    out->allocation_protect = e->w32_allocation_protect;
    out->state = e->kind == UA_W32 ? e->w32_state : W32_MEM_COMMIT;
    out->protect = e->w32_protect;
    out->type = e->kind == UA_W32 ? e->w32_type : W32_MEM_MAPPED;
    out->region_size = end - base;
    return 0;
}

int w32_map_system(struct uaddr *u, uint32_t bytes, uint32_t owner, uint32_t *base)
{
    struct ciuki_w32_vm a = { .bytes = bytes, .flags = W32_MEM_RESERVE | W32_MEM_COMMIT,
        .protect = W32_PAGE_READWRITE };
    uint32_t status = w32_vm_alloc(u, &a);
    if (status) return status == W32_STATUS_NO_MEMORY ? -ENOMEM : -EINVAL;
    for (struct ua_extent *e = ua_find(u, a.address); e && e->w32_allocation == a.address; e = e->next) {
        e->w32_locked = true; e->owner = owner;
    }
    *base = a.address;
    return 0;
}

uint32_t w32_guard_fault(struct proc_thread *t, uint32_t address, bool *retry)
{
    struct uaddr *u = t->process->memory;
    struct ua_extent *e = ua_find(u, address);
    *retry = false;
    if (!e || e->w32_state != W32_MEM_COMMIT || !(e->w32_protect & W32_PAGE_GUARD))
        return W32_STATUS_ACCESS_VIOLATION;
    uint32_t page = address & ~(PAGE_SIZE - 1), protect = e->w32_protect & ~W32_PAGE_GUARD, native;
    w32_protection(protect, &native);
    if (ua_w32_change(u, page, PAGE_SIZE, W32_VM_PROTECT, protect, native, true)) return W32_EXCEPTION_STACK;
    if (page != t->stack_guard) return W32_EXCEPTION_GUARD;
    if (page <= t->stack_base + PAGE_SIZE) {
        t->stack_guard = 0; t->stack_limit = page;
        ua_write(u, t->teb + 8, &page, sizeof(page));
        return W32_EXCEPTION_STACK;
    }
    uint32_t next = page - PAGE_SIZE;
    if (ua_w32_change(u, next, PAGE_SIZE, W32_VM_ALLOC, W32_PAGE_READWRITE | W32_PAGE_GUARD, 0, true))
        return W32_EXCEPTION_STACK;
    t->stack_guard = next; t->stack_limit = page;
    ua_write(u, t->teb + 8, &page, sizeof(page));
    *retry = true;
    return 0;
}
