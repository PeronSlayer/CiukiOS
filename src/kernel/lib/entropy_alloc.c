/* Jitterentropy's buffers exceed the small heap's 2 KiB classes. Use
 * existing owned PMM runs for those buffers, without changing heap limits.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/entropy.h>
struct entropy_allocation { uint32_t pages, reserved[3]; };
void *entropy_alloc(size_t size)
{
    if (!size || size > 262144) return NULL;
    size_t total = size + sizeof(struct entropy_allocation);
    struct entropy_allocation *h;
    if (total <= 2040) {
        h = kmalloc(total);
        if (!h) return NULL;
        memset(h, 0, sizeof(*h));
    } else {
        unsigned pages = PAGE_ALIGN_UP(total) / PAGE_SIZE;
        uint32_t phys = pmm_alloc_contig(pages);
        if (!phys) return NULL;
        mm_claim(phys, pages, MM_HEAP);
        h = P2V(phys);
        *h = (struct entropy_allocation){.pages = pages};
    }
    return h + 1;
}
void entropy_free(void *buffer)
{
    if (!buffer) return;
    struct entropy_allocation *h = (struct entropy_allocation *)buffer - 1;
    unsigned pages = h->pages;
    if (!pages) { kfree(h); return; }
    uint32_t phys = V2P(h);
    for (unsigned i = 0; i < pages; i++) mm_free(phys + i * PAGE_SIZE, MM_HEAP);
}
