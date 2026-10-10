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
    pool_pages++;
    uint8_t *page = P2V(p);
    unsigned sz = class_size(c);
    for (unsigned off = 0; off + sz <= PAGE_SIZE; off += sz) {
        *(void **)(page + off) = freelist[c];
        freelist[c] = page + off;
    }
    return true;
}

void *kmalloc(size_t size)
{
    size_t need = size + sizeof(struct hdr);
    unsigned c = 0;
    while (c < NCLASS && class_size(c) < need)
        c++;
    if (c == NCLASS)
        return 0;
    uint32_t f = irq_save();
    if (!freelist[c] && !refill(c)) {
        irq_restore(f);
        return 0;
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
    *out = (struct kheap_ledger){.pages=pool_pages,.in_use=in_use,.peak=peak};
    irq_restore(f);
}
