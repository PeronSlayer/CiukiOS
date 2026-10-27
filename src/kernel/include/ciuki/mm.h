/* Memory management interfaces (docs/design/boot-memory.md).
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_MM_H
#define CIUKI_MM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <ciuki/ledger.h>
#include <ciuki/kheap_limits.h>
#include <ciuki/memory_limits.h>

#define ZONE_LOW        0
#define ZONE_DMA        1
#define ZONE_NORMAL     2

/* Resident PFN metadata. PFN is the array index, never a duplicated address.
 * Reserved for future verified page-in: slot, io_in_flight and waiters. */
enum mm_backing_kind { MM_BACKING_PRIVATE, MM_BACKING_SHARED, MM_BACKING_ZERO,
                       MM_BACKING_FILE, MM_BACKING_DEVICE, MM_BACKING_SWAP };
struct mm_page_descriptor {
    uint32_t refs, accessed_epoch, dirty_epoch, backing_slot;
    uint16_t pins, waiters;
    uint8_t backing_kind, io_in_flight;
    uint16_t reserved;
};
struct mm_page_descriptor *pmm_descriptor(uint32_t phys) __attribute__((weak));
uint32_t pmm_descriptor_count(void);
/* reserved packs physical ownership and sampled temperature, not aliases. */
#define MM_PAGE_OWNER_MASK 31u
#define MM_PAGE_HOT 32u
#define MM_PAGE_TOUCHED 64u
void ledger_page_activity(uint32_t phys, bool hot) __attribute__((weak));

/* physical pages */
void pmm_init(void);
uint32_t pmm_alloc(void);                  /* NORMAL then DMA; returns phys or 0 */
uint32_t pmm_alloc_zone(int zone);
uint32_t pmm_alloc_contig(uint32_t pages); /* contiguous run, NORMAL then DMA; phys or 0; free with pmm_free per page */
/* Constrained, aligned DMA run; alignment is a power of two in pages.
 * Never spills above the ISA 24-bit address limit. */
uint32_t pmm_alloc_dma_run(uint32_t pages, uint32_t alignment);
void pmm_free(uint32_t phys);
/* Paired with mm_claim: return known object ownership before PMM release. */
void pmm_free_owned(uint32_t phys, enum mm_owner owner) __attribute__((weak));
static inline void mm_free(uint32_t phys, enum mm_owner owner)
{
    if (pmm_free_owned) pmm_free_owned(phys, owner);
    else pmm_free(phys); /* fake-PMM host fixtures */
}
uint32_t pmm_free_count(void);
uint32_t pmm_refusals(void);           /* fx-selftest: public allocations that returned 0 */
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
#define PTE_A    0x020u
#define PTE_D    0x040u
#define PTE_G    0x100u
/* Only for raw device address spaces (as.memory == NULL). Not a UA shared
 * page: this alias never owns, releases, copies or changes the backing. */
#define PTE_BORROWED 0x800u

/* kernel virtual memory */
void vmm_init(void);
uint32_t vmm_kernel_pd(void);              /* physical address */
void *vmm_map_mmio(uint32_t phys, uint32_t bytes, bool uncached);
/* Borrow firmware bytes through a supervisor read-only window. The caller
 * validates physical ownership and revokes with vmm_revoke_mmio afterward. */
const void *vmm_map_physical_ro(uint32_t phys, uint32_t bytes);
/* A reusable, private MMIO window; reserve once, revoke before firmware,
 * then replace only after validating device ownership and metadata. */
void *vmm_reserve_mmio(uint32_t capacity);
void vmm_revoke_mmio(void *mapped, uint32_t bytes);
void *vmm_replace_mmio(void *window, uint32_t capacity, uint32_t phys, uint32_t bytes);

/* Initial boot LFB aliases: configure MTRR before its first mapping, then
 * give console and presenter identical page attributes (WC or UC fallback). */
void *vmm_map_framebuffer(uint32_t phys, uint32_t bytes);
/* All console/presenter aliases must be revoked before runtime replacement. */
void *vmm_replace_framebuffer(void *window, uint32_t capacity, uint32_t phys, uint32_t bytes);
uint32_t vmm_kernel_pte(uint32_t va);      /* raw PTE or 0 */
/* Thread context, before publishing a device view. Refuse mixed PCD/PWT
 * aliases, including the permanent direct map. No mapping is changed. */
bool vmm_cache_alias_ok(uint32_t physical,uint32_t bytes,uint32_t flags);

/* kernel stacks: 16 KiB with an unmapped guard page below (f2-20) */
#define KSTACK_SIZE 32768u /* 32 KiB: the optical probe path double-faulted at 16 KiB on QEMU (2026-10-11) */
void *kstack_alloc(void);                  /* returns lowest usable address */
void kstack_free(void *base);
uint32_t kstack_guard_va(void *base);

/* address spaces */
struct aspace {
    uint32_t pd_phys;
    uint32_t pages;                        /* user pages mapped */
    uint32_t tables;                       /* user page tables allocated */
    enum mm_owner owner;                  /* data frames; tables are MM_TABLE */
    struct uaddr *memory;                 /* object-backed areas, NULL for VM/device */
    uint32_t pool_base;                    /* VM pool boundary, zero otherwise */
    struct aspace *sample_prev, *sample_next; /* resident registry, UP exclusion */
};
/* One PDE skip or PTE sample, including holes, per call. No object pointers
 * escape IRQ exclusion. Returns true at the end of one global alias pass. */
bool vmm_sample_step(uint32_t epoch) __attribute__((weak));
uint32_t as_sample_page(struct aspace *, uint32_t va);
int as_create(struct aspace *as);
int as_map(struct aspace *as, uint32_t va, uint32_t phys, uint32_t flags);
int as_map_borrowed(struct aspace *, uint32_t va, uint32_t phys, uint32_t flags);
int as_revoke_borrowed(struct aspace *, uint32_t va, uint32_t phys);
/* Drop write access on an exact borrowed alias; never changes cache/ownership. */
int as_seal_borrowed(struct aspace *, uint32_t va, uint32_t phys);
/* Replace 1..32 already-present pages within one table; no allocation or
 * ownership change. Caller excludes guest execution/interrupts. Return old
 * D/A bits as page masks, clear D, and INVLPG each changed entry of the
 * active AS (fx-27: never a whole-TLB CR3 reload) whose replaced value had
 * A or D set (fx-doswin3; Intel SDM Vol. 3A 5.10.2.3, 5.10.4.3). Valid only
 * on one CPU with CR4.PGE and CR4.PCIDE clear, with every A 1->0 writer
 * invalidating while its space is current (vmm.c as_replace_pages).
 * frames == NULL keeps frames/permissions (dirty collection only). */
int as_replace_pages(struct aspace *, uint32_t va, const uint32_t *frames,
                     unsigned count, uint32_t flags, uint32_t *dirty, uint32_t *accessed);
uint32_t as_lookup(const struct aspace *as, uint32_t va, uint32_t *pte_out);
void as_destroy(struct aspace *as);        /* frees user pages and tables */
bool as_range_ok(const struct aspace *as, uint32_t va, uint32_t len, bool write);

/* kernel heap */
void kheap_init(void);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
/* Keep the real symbols for allocator definitions and runtime boundary tests.
 * sizeof never evaluates size: even a side-effecting argument runs only once.
 * A negative array bound rejects oversized constants at every optimization level. */
#define KHEAP_CHECK_SIZE(size) \
    ((void)sizeof(char[__builtin_constant_p(size) ? \
        ((size_t)(size) <= KHEAP_MAX_REQUEST ? 1 : -1) : 1]))
#ifndef kmalloc
#define kmalloc(size) (KHEAP_CHECK_SIZE(size), (kmalloc)(size))
#endif
#ifndef kzalloc
#define kzalloc(size) (KHEAP_CHECK_SIZE(size), (kzalloc)(size))
#endif
void kfree(void *p);
size_t kheap_in_use(void);
/* Class-pool pages retained for reuse; class bytes include headers/padding.
 * Peak is the maximum bytes in use since kheap_init(). Read-only snapshot. */
struct kheap_ledger {
    uint32_t pages;
    size_t in_use, peak;
    uint32_t refused; /* fx-selftest: kmalloc returned NULL (oversize or no page) */
};
void kheap_snapshot(struct kheap_ledger *out);

#endif
