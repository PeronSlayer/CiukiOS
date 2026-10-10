/* Production stack allocator with a fake page/PTE boundary: rollback at
 * every allocation, guard isolation, all mapped pages freed, slot reuse.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <ciuki/kernel.h>
#include <ciuki/mm.h>
#define STACK_REGION 0xF0000000u
#define STACK_SLOT (PAGE_SIZE + KSTACK_SIZE)
#define STACK_SLOTS 1360u
static uint8_t stack_slot_used[STACK_SLOTS];
static uint32_t ptes[STACK_SLOTS * (1 + KSTACK_SIZE / PAGE_SIZE)];
static bool used[32];
static unsigned live, maps, flushes;
static int fail_after = -1;
uint32_t pmm_alloc(void)
{
    if (!fail_after) return 0;
    if (fail_after > 0) fail_after--;
    for (unsigned i = 1; i < ARRAY_SIZE(used); i++) if (!used[i]) {
        used[i] = true; live++; return i * PAGE_SIZE;
    }
    return 0;
}
void pmm_free(uint32_t phys)
{
    assert(phys && !(phys % PAGE_SIZE) && phys / PAGE_SIZE < ARRAY_SIZE(used));
    assert(used[phys / PAGE_SIZE]);
    used[phys / PAGE_SIZE] = false; live--;
}
static unsigned index_of(uint32_t va)
{
    assert(va >= STACK_REGION && !(va % PAGE_SIZE));
    unsigned index = (va - STACK_REGION) / PAGE_SIZE;
    assert(index < ARRAY_SIZE(ptes)); return index;
}
static void kmap(uint32_t va, uint32_t phys, uint32_t flags)
{ assert(!ptes[index_of(va)]); ptes[index_of(va)] = phys | flags | PTE_P; maps++; }
static void kunmap(uint32_t va) { ptes[index_of(va)] = 0; }
static void invlpg(uint32_t va) { (void)index_of(va); flushes++; }
uint32_t vmm_kernel_pte(uint32_t va) { return ptes[index_of(va)]; }
void panic(const char *fmt, ...) { (void)fmt; abort(); }
#include "stack_allocator.inc"
int main(void)
{
    unsigned pages = KSTACK_SIZE / PAGE_SIZE;
    assert(pages == 4);
    for (unsigned i = 0; i < pages; i++) {
        fail_after = (int)i;
        assert(!kstack_alloc() && !live && !maps && !flushes && !stack_slot_used[0]);
        for (unsigned n = 0; n < ARRAY_SIZE(ptes); n++) assert(!ptes[n]);
    }
    fail_after = -1;
    void *a = kstack_alloc(), *b = kstack_alloc();
    assert(a && b && (uintptr_t)b - (uintptr_t)a == STACK_SLOT && live == 2 * pages);
    assert(!vmm_kernel_pte(kstack_guard_va(a)) && !vmm_kernel_pte(kstack_guard_va(b)));
    for (unsigned i = 0; i < pages; i++) {
        uint32_t pte = vmm_kernel_pte((uint32_t)(uintptr_t)a + i * PAGE_SIZE);
        assert((pte & (PTE_P | PTE_W)) == (PTE_P | PTE_W) && !(pte & PTE_U));
    }
    kstack_free(a);
    assert(live == pages);
    for (unsigned i = 0; i <= pages; i++) assert(!ptes[i]);
    void *again = kstack_alloc(); assert(again == a && live == 2 * pages);
    kstack_free(b); kstack_free(again);
    assert(!live);
    for (unsigned i = 0; i < ARRAY_SIZE(ptes); i++) assert(!ptes[i]);
    for (unsigned i = 0; i < STACK_SLOTS; i++) stack_slot_used[i] = 1;
    assert(!kstack_alloc() && !live);
    puts("stack allocator: PASS (4 pages, failure rollback, unmapped guards, release, reuse, slots)");
    return 0;
}
