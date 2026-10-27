/* Small kernel heap: power-of-two size classes up to 2 KiB on pages from
 * the physical allocator, reached through the direct map.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/mm.h>
#include <ciuki/cpu.h>

#define NCLASS 7                          /* 32 .. 2048 bytes incl. header */
struct hdr { uint32_t cls; uint32_t magic; };
#define HDR_MAGIC 0xC1A0F00Du

static void *freelist[NCLASS];
static size_t in_use;
static size_t peak;
static uint32_t pool_pages;
static uint32_t refused; /* fx-selftest: oversize or failed refill */

static unsigned class_size(unsigned c) { return 32u << c; }

void kheap_init(void)
{
    for (unsigned i = 0; i < NCLASS; i++)
        freelist[i] = 0;
    in_use = 0;
    peak = 0;
    pool_pages = 0;
}

static bool refill(unsigned c)
{
    uint32_t p = pmm_alloc();
    if (!p)
        return false;
    mm_claim(p,1,MM_HEAP);
    uint8_t *page = P2V(p);
    unsigned sz = class_size(c);
    void *first = 0, *last = 0;
    /* At most 128 slots * 1 us = 128 us, constructed privately before
     * the constant-time IRQ-off publication. No reclaim walk exists. */
    for (unsigned off = 0; off + sz <= PAGE_SIZE; off += sz) {
        if (!last) last = page + off;
        *(void **)(page + off) = first;
        first = page + off;
    }
    uint32_t f = irq_save();
    pool_pages++;
    *(void **)last = freelist[c];
    freelist[c] = first;
    irq_restore(f);
    return true;
}

void *kmalloc(size_t size)
{
    if (ledger_assert_pinned_context) ledger_assert_pinned_context(true,(uintptr_t)__builtin_return_address(0));
    size_t need = size + sizeof(struct hdr);
    unsigned c = 0;
    while (c < NCLASS && class_size(c) < need)
        c++;
    if (c == NCLASS) {
        __atomic_add_fetch(&refused, 1, __ATOMIC_RELAXED);
        return 0;
    }
    uint32_t f = irq_save();
    if (!freelist[c]) {
        irq_restore(f);
        if (!refill(c)) { __atomic_add_fetch(&refused, 1, __ATOMIC_RELAXED); return 0; }
        f = irq_save();
    }
    struct hdr *h = freelist[c];
    freelist[c] = *(void **)h;
    h->cls = c;
    h->magic = HDR_MAGIC;
    in_use += class_size(c);
    if (in_use > peak)
        peak = in_use;
    irq_restore(f);
    return h + 1;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

void kfree(void *p)
{
    if (!p)
        return;
    struct hdr *h = (struct hdr *)p - 1;
    if (h->magic != HDR_MAGIC || h->cls >= NCLASS)
        panic("kfree: corrupt block %p", p);
    uint32_t f = irq_save();
    unsigned c = h->cls;
    h->magic = 0;
    in_use -= class_size(c);
    *(void **)h = freelist[c];
    freelist[c] = h;
    irq_restore(f);
}

size_t kheap_in_use(void) { return in_use; }

void kheap_snapshot(struct kheap_ledger *out)
{
    uint32_t f = irq_save();
    *out = (struct kheap_ledger){.pages=pool_pages,.in_use=in_use,.peak=peak,.refused=refused};
    irq_restore(f);
}
