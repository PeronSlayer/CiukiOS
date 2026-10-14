/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. Original Win32 heap implementation.
 *
 * Segregated free lists with boundary headers. A block pointer is validated in
 * O(segments) from its own header (magic, owning segment and physical-neighbour
 * links) instead of walking every block. Freed payload bytes are preserved
 * unless HEAP_FREE_CHECKING_ENABLED asks for the debug fill: titles of the era
 * read freed blocks (contract, "Sections, heaps and file mappings"). */
#include <w32/ntdll.h>
#define HEAP_MAGIC 0x43554850u
#define BLOCK_MAGIC 0x4355424cu
#define MAX_HEAP_SIZE 0x20000000u
/* NT x86 VirtualAllocThreshold: larger requests get a private segment that is
 * returned to the system when the block is freed. */
#define LARGE_BLOCK 0x7f000u
#define EXACT_BINS 64
#define BINS 96
struct block {
    struct block *next, *prev;           /* physical neighbours in the segment */
    struct block *free_next, *free_prev; /* bin links while free */
    ULONG magic, capacity, size, used;
};
struct segment {
    struct segment *next;
    ULONG bytes;
    struct block *first;
    ULONG padding;
};
struct heap {
    ULONG magic, flags, maximum, total;
    RTL_CRITICAL_SECTION lock;
    struct segment *segments;
    struct block *bins[BINS];
    ULONG map[(BINS + 31) / 32];
};
_Static_assert(sizeof(struct block) % 8 == 0 && sizeof(struct segment) % 8 == 0,
               "8-byte heap alignment");
static ULONG bin_of(ULONG capacity) {
    if (capacity < EXACT_BINS * 8)
        return capacity / 8;
    ULONG b = EXACT_BINS;
    for (ULONG c = capacity >> 10; c && b < BINS - 1; c >>= 1)
        b++;
    return b;
}
static void bin_insert(struct heap *h, struct block *b) {
    ULONG i = bin_of(b->capacity);
    b->free_prev = 0;
    b->free_next = h->bins[i];
    if (b->free_next)
        b->free_next->free_prev = b;
    h->bins[i] = b;
    h->map[i / 32] |= 1u << (i % 32);
}
static void bin_remove(struct heap *h, struct block *b) {
    ULONG i = bin_of(b->capacity);
    if (b->free_prev)
        b->free_prev->free_next = b->free_next;
    else
        h->bins[i] = b->free_next;
    if (b->free_next)
        b->free_next->free_prev = b->free_prev;
    b->free_next = b->free_prev = 0;
    if (!h->bins[i])
        h->map[i / 32] &= ~(1u << (i % 32));
}
static struct block *bin_take(struct heap *h, ULONG cap) {
    for (ULONG i = bin_of(cap); i < BINS; i++) {
        if (!(h->map[i / 32] & (1u << (i % 32)))) {
            if (!(i % 32) && !h->map[i / 32])
                i += 31;
            continue;
        }
        for (struct block *b = h->bins[i]; b; b = b->free_next)
            if (b->capacity >= cap) {
                bin_remove(h, b);
                return b;
            }
    }
    return 0;
}
static int lock_heap(struct heap *h, ULONG flags) {
    if (!h || h->magic != HEAP_MAGIC)
        return 0;
    return ((h->flags | flags) & HEAP_NO_SERIALIZE) || !RtlEnterCriticalSection(&h->lock);
}
static void unlock_heap(struct heap *h, ULONG flags) {
    if (!((h->flags | flags) & HEAP_NO_SERIALIZE))
        RtlLeaveCriticalSection(&h->lock);
}
static struct segment *owner(struct heap *h, const struct block *b) {
    for (struct segment *s = h->segments; s; s = s->next) {
        uintptr_t lo = (uintptr_t)(s + 1), hi = (uintptr_t)s + s->bytes;
        if ((uintptr_t)b >= lo && (uintptr_t)b < hi && hi - (uintptr_t)b >= sizeof(*b))
            return s;
    }
    return 0;
}
/* Header-based lookup; rejects interior and foreign pointers. */
static struct block *find(struct heap *h, const void *p) {
    if (!p || ((uintptr_t)p & 7))
        return 0;
    struct block *b = (struct block *)p - 1;
    struct segment *s = owner(h, b);
    if (!s || b->magic != BLOCK_MAGIC)
        return 0;
    if (b->prev ? (owner(h, b->prev) != s || b->prev->next != b) : b != s->first)
        return 0;
    if (b->next && (owner(h, b->next) != s || b->next->prev != b))
        return 0;
    return b;
}
static int tail_ok(struct heap *h, struct block *b) {
    if (!(h->flags & HEAP_TAIL_CHECKING_ENABLED) || !b->used)
        return 1;
    uint8_t *p = (uint8_t *)(b + 1) + b->size;
    for (unsigned i = 0; i < 8; i++)
        if (p[i] != 0xab)
            return 0;
    return 1;
}
static struct segment *grow(struct heap *h, ULONG capacity) {
    const struct w32_services *v = w32_get_services();
    if (capacity > MAX_HEAP_SIZE - sizeof(struct segment) - sizeof(struct block) - 4095)
        return 0;
    ULONG bytes = (capacity + sizeof(struct segment) + sizeof(struct block) + 4095) & ~4095u;
    if (h->maximum && bytes > h->maximum - h->total)
        return 0;
    struct segment *s = v->alloc(v->opaque, bytes);
    if (!s)
        return 0;
    w32_fill(s, 0, sizeof(*s));
    s->bytes = bytes;
    s->next = h->segments;
    s->first = (struct block *)(s + 1);
    w32_fill(s->first, 0, sizeof(struct block));
    s->first->magic = BLOCK_MAGIC;
    s->first->capacity = bytes - sizeof(*s) - sizeof(struct block);
    h->segments = s;
    h->total += bytes;
    bin_insert(h, s->first);
    return s;
}
static void release_segment(struct heap *h, struct segment *s) {
    const struct w32_services *v = w32_get_services();
    struct segment **link = &h->segments;
    while (*link && *link != s)
        link = &(*link)->next;
    if (!*link)
        return;
    *link = s->next;
    h->total -= s->bytes;
    v->free(v->opaque, s, s->bytes);
}
/* Split a used block; the remainder becomes a free block in its bin. */
static void split(struct heap *h, struct block *b, ULONG cap) {
    if (b->capacity < cap + sizeof(*b) + 8)
        return;
    struct block *n = (struct block *)((uint8_t *)(b + 1) + cap);
    w32_fill(n, 0, sizeof(*n));
    n->magic = BLOCK_MAGIC;
    n->capacity = b->capacity - cap - sizeof(*n);
    n->next = b->next;
    n->prev = b;
    if (n->next)
        n->next->prev = n;
    b->next = n;
    b->capacity = cap;
    if (n->next && !n->next->used) {
        struct block *m = n->next;
        bin_remove(h, m);
        n->capacity += sizeof(*m) + m->capacity;
        n->next = m->next;
        if (n->next)
            n->next->prev = n;
        m->magic = 0;
    }
    bin_insert(h, n);
}
/* Absorb free physical successors of b (b itself is not in a bin). */
static void absorb(struct heap *h, struct block *b) {
    while (b->next && !b->next->used) {
        struct block *n = b->next;
        bin_remove(h, n);
        b->capacity += sizeof(*n) + n->capacity;
        b->next = n->next;
        if (b->next)
            b->next->prev = b;
        n->magic = 0;
    }
}
static void stamp(struct heap *h, struct block *b) {
    if (h->flags & HEAP_TAIL_CHECKING_ENABLED)
        w32_fill((uint8_t *)(b + 1) + b->size, 0xab, 8);
}
static void *failure(ULONG flags) {
    if (flags & HEAP_GENERATE_EXCEPTIONS)
        RtlRaiseStatus(STATUS_NO_MEMORY);
    return 0;
}
static ULONG capacity_for(struct heap *h, uint32_t size) {
    ULONG tail = (h->flags & HEAP_TAIL_CHECKING_ENABLED) ? 8 : 0;
    return ((size ? size : 1) + tail + 7) & ~7u;
}
void *W32API RtlCreateHeap(ULONG flags, void *base, uint32_t reserve, uint32_t commit, void *lock,
                           void *params) {
    const struct w32_services *v = w32_get_services();
    /* Externally supplied regions/locks and NT tuning parameters require a
     * section service; refuse instead of silently ignoring their ownership. */
    if (base || lock || params || !v || !v->alloc || !v->free || reserve > MAX_HEAP_SIZE ||
        commit > MAX_HEAP_SIZE)
        return 0;
    struct heap *h = v->alloc(v->opaque, sizeof(*h));
    if (!h)
        return 0;
    w32_fill(h, 0, sizeof(*h));
    h->magic = HEAP_MAGIC;
    h->flags = flags;
    if (!(flags & HEAP_GROWABLE) && reserve)
        h->maximum = (reserve + 4095) & ~4095u;
    RtlInitializeCriticalSection(&h->lock);
    ULONG first = commit ? commit : 4096;
    if (h->maximum && first > h->maximum) {
        v->free(v->opaque, h, sizeof(*h));
        return 0;
    }
    /* grow() adds segment metadata; choose payload so initial commit fits. */
    if (!grow(h, first > sizeof(struct segment) + sizeof(struct block)
                     ? first - sizeof(struct segment) - sizeof(struct block)
                     : 8)) {
        v->free(v->opaque, h, sizeof(*h));
        return 0;
    }
    return h;
}
void *W32API RtlDestroyHeap(void *p) {
    struct heap *h = p;
    const struct w32_services *v = w32_get_services();
    if (!h || h->magic != HEAP_MAGIC)
        return p;
    /* The caller must ensure no concurrent users, as HeapDestroy requires. */
    struct segment *s = h->segments;
    h->magic = 0;
    while (s) {
        struct segment *next = s->next;
        v->free(v->opaque, s, s->bytes);
        s = next;
    }
    v->free(v->opaque, h, sizeof(*h));
    return 0;
}
static void *allocate_locked(struct heap *h, ULONG flags, uint32_t size) {
    ULONG combined = flags | h->flags;
    if (size > MAX_HEAP_SIZE - 16)
        return 0;
    ULONG cap = capacity_for(h, size);
    struct block *b = cap >= LARGE_BLOCK ? 0 : bin_take(h, cap);
    if (!b) {
        struct segment *s = grow(h, cap > 65536 - 64 ? cap : 65536 - 64);
        if (!s)
            return 0;
        b = s->first;
        bin_remove(h, b);
    }
    b->used = 1;
    split(h, b, cap);
    b->size = size;
    if (combined & HEAP_ZERO_MEMORY)
        w32_fill(b + 1, 0, size);
    else if (h->flags & HEAP_FREE_CHECKING_ENABLED) {
        uint8_t *d = (void *)(b + 1);
        static const uint8_t fill[] = {0x0d, 0xf0, 0xad, 0xba};
        for (ULONG i = 0; i < size; i++)
            d[i] = fill[i & 3];
    }
    stamp(h, b);
    return b + 1;
}
static void free_locked(struct heap *h, struct block *b) {
    if (h->flags & HEAP_FREE_CHECKING_ENABLED) {
        uint8_t *d = (void *)(b + 1);
        static const uint8_t fill[] = {0xee, 0xfe, 0xee, 0xfe};
        for (ULONG i = 0; i < b->size; i++)
            d[i] = fill[i & 3];
    }
    b->used = 0;
    b->size = 0;
    absorb(h, b);
    if (b->prev && !b->prev->used) {
        struct block *p = b->prev;
        bin_remove(h, p);
        p->capacity += sizeof(*b) + b->capacity;
        p->next = b->next;
        if (p->next)
            p->next->prev = p;
        b->magic = 0;
        b = p;
    }
    struct segment *s = owner(h, b), *initial = h->segments;
    while (initial && initial->next)
        initial = initial->next;
    /* Return wholly free extra segments (large blocks, growth); keep the
     * initial commit, which is the oldest segment. */
    if (s && s != initial && b == s->first && !b->next) {
        release_segment(h, s);
        return;
    }
    bin_insert(h, b);
}
void *W32API RtlAllocateHeap(void *p, ULONG flags, uint32_t size) {
    struct heap *h = p;
    if (!lock_heap(h, flags))
        return 0;
    ULONG combined = flags | h->flags;
    void *result = allocate_locked(h, flags, size);
    unlock_heap(h, flags);
    return result ? result : failure(combined);
}
BOOLEAN W32API RtlFreeHeap(void *p, ULONG flags, void *data) {
    struct heap *h = p;
    if (!lock_heap(h, flags))
        return 0;
    if (!data) {
        unlock_heap(h, flags);
        return 1;
    }
    struct block *b = find(h, data);
    if (!b || !b->used || !tail_ok(h, b)) {
        unlock_heap(h, flags);
        return 0;
    }
    free_locked(h, b);
    unlock_heap(h, flags);
    return 1;
}
void *W32API RtlReAllocateHeap(void *p, ULONG flags, void *data, uint32_t size) {
    struct heap *h = p;
    if (!data || !lock_heap(h, flags))
        return 0;
    struct block *b = find(h, data);
    ULONG combined = h->flags | flags;
    if (!b || !b->used || !tail_ok(h, b) || size > MAX_HEAP_SIZE - 16) {
        unlock_heap(h, flags);
        return size > MAX_HEAP_SIZE - 16 && b ? failure(combined) : 0;
    }
    ULONG old = b->size, cap = capacity_for(h, size);
    ULONG available = b->capacity;
    for (struct block *n = b->next; n && !n->used && available < cap; n = n->next)
        available += sizeof(*n) + n->capacity;
    if (available >= cap) {
        if (b->capacity < cap)
            absorb(h, b);
        split(h, b, cap);
        b->size = size;
        if (size > old && (combined & HEAP_ZERO_MEMORY))
            w32_fill((uint8_t *)data + old, 0, size - old);
        stamp(h, b);
        unlock_heap(h, flags);
        return data;
    }
    if (flags & HEAP_REALLOC_IN_PLACE_ONLY) {
        unlock_heap(h, flags);
        return failure(combined);
    }
    uint8_t *q = allocate_locked(h, flags & ~HEAP_ZERO_MEMORY, size);
    if (q) {
        w32_copy(q, data, old < size ? old : size);
        if (size > old && (combined & HEAP_ZERO_MEMORY))
            w32_fill(q + old, 0, size - old);
        free_locked(h, b);
    }
    unlock_heap(h, flags);
    return q ? q : failure(combined);
}
uint32_t W32API RtlSizeHeap(void *p, ULONG flags, const void *data) {
    struct heap *h = p;
    if (!lock_heap(h, flags))
        return 0xffffffffu;
    struct block *b = find(h, data);
    ULONG n = b && b->used ? b->size : 0xffffffffu;
    unlock_heap(h, flags);
    return n;
}
BOOLEAN W32API RtlValidateHeap(void *p, ULONG flags, const void *data) {
    struct heap *h = p;
    if (!lock_heap(h, flags))
        return 0;
    int ok = 1;
    if (data) {
        struct block *b = find(h, data);
        ok = b && b->used && tail_ok(h, b);
    } else {
        ULONG free_blocks = 0, binned = 0;
        for (struct segment *s = h->segments; s && ok; s = s->next) {
            ULONG covered = sizeof(*s);
            for (struct block *b = s->first, *prev = 0; b; prev = b, b = b->next) {
                if (b->magic != BLOCK_MAGIC || b->prev != prev || owner(h, b) != s ||
                    (b->used && (b->size > b->capacity || !tail_ok(h, b))) ||
                    (!b->used && b->next && !b->next->used)) {
                    ok = 0;
                    break;
                }
                covered += sizeof(*b) + b->capacity;
                free_blocks += !b->used;
            }
            if (ok && covered != s->bytes)
                ok = 0;
        }
        for (ULONG i = 0; i < BINS && ok; i++)
            for (struct block *b = h->bins[i]; b; b = b->free_next) {
                if (b->used || b->magic != BLOCK_MAGIC || bin_of(b->capacity) != i ||
                    binned > free_blocks) {
                    ok = 0;
                    break;
                }
                binned++;
            }
        if (binned != free_blocks)
            ok = 0;
    }
    unlock_heap(h, flags);
    return ok;
}
BOOLEAN W32API RtlLockHeap(void *p) {
    struct heap *h = p;
    return h && h->magic == HEAP_MAGIC && !RtlEnterCriticalSection(&h->lock);
}
BOOLEAN W32API RtlUnlockHeap(void *p) {
    struct heap *h = p;
    return h && h->magic == HEAP_MAGIC && !RtlLeaveCriticalSection(&h->lock);
}
/* Walk order: for each segment, a segment entry (RTL_HEAP_SEGMENT) followed
 * by its blocks, busy (RTL_HEAP_BUSY) or free (0). */
NTSTATUS W32API RtlWalkHeap(void *p, struct w32_heap_entry *entry) {
    struct heap *h = p;
    if (!entry || !lock_heap(h, 0))
        return STATUS_INVALID_PARAMETER;
    NTSTATUS result = (NTSTATUS)0x8000001a; /* STATUS_NO_MORE_ENTRIES */
    struct segment *s = h->segments;
    struct block *b = 0;
    if (entry->DataAddress) {
        if (entry->Flags & 2) {
            for (s = h->segments; s && (void *)s != entry->DataAddress; s = s->next)
                ;
            if (!s) {
                unlock_heap(h, 0);
                return STATUS_INVALID_PARAMETER;
            }
            b = s->first;
        } else {
            struct block *c = find(h, entry->DataAddress);
            if (!c) {
                unlock_heap(h, 0);
                return STATUS_INVALID_PARAMETER;
            }
            s = owner(h, c);
            b = c->next;
            if (!b)
                s = s->next;
        }
    }
    if (!b && s) {
        w32_fill(entry, 0, sizeof(*entry));
        entry->DataAddress = s;
        entry->DataSize = s->bytes;
        entry->OverheadBytes = sizeof(*s);
        entry->Flags = 2;
        entry->u.Segment.CommittedSize = s->bytes;
        entry->u.Segment.FirstEntry = s->first;
        entry->u.Segment.LastEntry = (uint8_t *)s + s->bytes;
        result = 0;
    } else if (b) {
        w32_fill(entry, 0, sizeof(*entry));
        entry->DataAddress = b + 1;
        entry->DataSize = b->used ? b->size : b->capacity;
        entry->OverheadBytes = sizeof(*b) + (b->used ? b->capacity - b->size : 0);
        entry->Flags = b->used ? 1 : 0;
        result = 0;
    }
    unlock_heap(h, 0);
    return result;
}
