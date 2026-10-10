/* Memory management interfaces (docs/design/boot-memory.md).
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_MM_H
#define CIUKI_MM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PMM_MAX_PAGES   (768u * 256u)      /* direct map limit: 768 MiB */
#define ZONE_LOW        0
#define ZONE_DMA        1
#define ZONE_NORMAL     2

/* physical pages */
void pmm_init(void);
uint32_t pmm_alloc(void);                  /* NORMAL then DMA; returns phys or 0 */
uint32_t pmm_alloc_zone(int zone);
void pmm_free(uint32_t phys);
uint32_t pmm_free_count(void);
uint32_t pmm_zone_free(int zone);
uint32_t pmm_zone_total(int zone);
uint32_t pmm_total_usable(void);
bool pmm_is_reserved(uint32_t phys);       /* true for any page never usable */
bool pmm_page_free(uint32_t pfn);
extern uint32_t g_user_mappings;          /* user PTEs currently present */
struct pmm_reservation { uint32_t start, end; const char *why; };
unsigned pmm_reservations(const struct pmm_reservation **out);

/* page table flags */
#define PTE_P    0x001u
#define PTE_W    0x002u
#define PTE_U    0x004u
#define PTE_PWT  0x008u
#define PTE_PCD  0x010u
#define PTE_G    0x100u

/* kernel virtual memory */
void vmm_init(void);
uint32_t vmm_kernel_pd(void);              /* physical address */
void *vmm_map_mmio(uint32_t phys, uint32_t bytes, bool uncached);
uint32_t vmm_kernel_pte(uint32_t va);      /* raw PTE or 0 */

/* kernel stacks: 16 KiB with an unmapped guard page below (f2-20) */
#define KSTACK_SIZE 16384u
void *kstack_alloc(void);                  /* returns lowest usable address */
void kstack_free(void *base);
uint32_t kstack_guard_va(void *base);

/* address spaces */
struct aspace {
    uint32_t pd_phys;
    uint32_t pages;                        /* user pages mapped */
    uint32_t tables;                       /* user page tables allocated */
};
int as_create(struct aspace *as);
int as_map(struct aspace *as, uint32_t va, uint32_t phys, uint32_t flags);
uint32_t as_lookup(const struct aspace *as, uint32_t va, uint32_t *pte_out);
void as_destroy(struct aspace *as);        /* frees user pages and tables */
bool as_range_ok(const struct aspace *as, uint32_t va, uint32_t len, bool write);

/* kernel heap */
void kheap_init(void);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void kfree(void *p);
size_t kheap_in_use(void);
/* Class-pool pages retained for reuse; class bytes include headers/padding.
 * Peak is the maximum bytes in use since kheap_init(). Read-only snapshot. */
struct kheap_ledger {
    uint32_t pages;
    size_t in_use, peak;
};
void kheap_snapshot(struct kheap_ledger *out);

#endif
