/* Per-VM XMS/EMS/UMB state. Guest addresses, never host physical addresses.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOSVM_MEMORY_H
#define CIUKI_DOSVM_MEMORY_H
#include <ciuki/reclaim.h>
#define VM_POOL_BASE 0x110000u
#define VM_POOL_CAP (24u * 1024u * 1024u)
#define VM_POOL_PAGES (VM_POOL_CAP / PAGE_SIZE)
#define VM_MEM_HANDLES 64u
#define VM_EMS_PAGE 16384u
#define VM_EMS_CAP (8u * 1024u * 1024u)
#define VM_XMS_SEG 0xC800u
#define VM_UMB_FIRST 0xC9000u
#define VM_UMB_END 0xE0000u
#define VM_EMS_CALLS 8u
struct vm_block { uint32_t base, bytes, pages; bool used; uint8_t locks; };
/* f3-46 (contract D1, docs/design/dos-dpmi-contract.md "Pageable pool"):
 * the XMS/EMS/DPMI pool is one memory object of the f0-47 model. Every
 * quota page is committed (RAM or pagefile backing promised) at creation;
 * a frame exists only while the page is resident; a pager slot holds a
 * paged-out page. XMS/EMS/DPMI handles are page ranges of this object; the
 * pool linear address and the EMS frame windows are aliases of one page
 * and share its PFN descriptor (access/dirty history). Pins keep device,
 * model, DMA and DPMI-locked pages resident. Locked mode (per-game
 * compatibility, PIF "XMS/EMS memory locked") is the eager, resident pool. */
#define VM_PAGE_ZERO 0u     /* committed, demand-zero: no frame, no copy */
#define VM_PAGE_RESIDENT 1u /* frame owned; translations may be revoked */
#define VM_PAGE_SWAPPED 2u  /* the pager slot holds the only copy */
#define VM_PAGE_BUSY 1u     /* flag: materialization or page-in in progress */
struct vm_frame;
struct vm_page {
    struct vm_frame *frame; /* reclaim tracker while resident (paged mode) */
    uint32_t phys, swap;    /* frame; pager slot while SWAPPED */
    uint16_t pins;          /* device/model/DMA/DPMI lock pins (resident) */
    uint8_t state, flags;
};
#define VM_POOL_PER_PAGE (PAGE_SIZE / sizeof(struct vm_page))
#define VM_POOL_DIR ((VM_POOL_PAGES + VM_POOL_PER_PAGE - 1) / VM_POOL_PER_PAGE)
#define VM_POOL_WINDOWS 16u /* EMS frame: four 16 KiB windows of four pages */
#define VM_POOL_DMA 8u
struct vm_pool_stats {
    uint32_t zero_fills, page_ins, page_in_errors, discards, frame_waits, busy_waits;
    uint32_t pageouts, clean_discards, redirtied, deferred, prewrites;
    uint32_t pin_failures, dma_waits, late_waits, waits, max_wait_us;
    uint64_t wait_us;
    /* Guest IRQ delay split (contract D1): pending while the worker waits
     * for page-in, pending with the guest's own virtual IF clear, pending
     * while the runnable worker is descheduled by the host. */
    uint64_t irq_page_ms, irq_guest_if_ms, irq_host_ms;
};
struct vm_pool {
    uint32_t dir[VM_POOL_DIR];          /* record pages (MM_VM_PRIVATE) */
    uint32_t pages, dir_pages;          /* address quota; record pages */
    uint32_t resident, swapped, pinned; /* pages; pinned = records with pins */
    uint32_t commit, reserve;           /* ledger pages charged (commit_ram, reserved_va) */
    uint32_t hold;                      /* monitor transaction depth: no page-out */
    uint32_t effects;                   /* device effects in the transaction */
    uint32_t window_base, window[VM_POOL_WINDOWS]; /* EMS alias: pool index + 1 */
    uint32_t dma_first[VM_POOL_DMA], dma_pages[VM_POOL_DMA]; /* pinned pool pages */
    bool locked, closing, committed, dma_dirty, waiting;
    struct vm_pool_stats stats;
};
struct vm_map { uint16_t handle, page; uint32_t serial; };
struct vm_ems_handle {
    struct vm_block block;
    uint32_t serial;
    char name[8];
    bool saved;
    struct vm_map map[4];
};
struct vm_ems_call { struct vm_map restore[4]; uint16_t ss, sp; };
struct vm_memory {
    uint32_t quota, used_pages, ems_bytes, serial;
    uint8_t occupied[VM_POOL_PAGES];
    struct vm_pool pool;
    struct vm_block xms[VM_MEM_HANDLES];
    struct vm_ems_handle ems[VM_MEM_HANDLES];
    struct vm_map map[4];
    struct vm_ems_call calls[VM_EMS_CALLS];
    unsigned depth;
    uint16_t umb[40]; /* allocation length at first page, continuation=FFFFh */
    uint16_t frame_segment, local_a20, global_a20;
    bool hma, umb_enabled;
};
int dosvm_memory_init(struct dosvm *, const struct dosvm_config *);
/* f3-46 pool object (dosvm_pool.c). Blocking calls run in task context
 * with interrupts enabled only, never inside an emulated instruction's
 * device effect; non-blocking callers see -EAGAIN for a missing frame. */
int dosvm_pool_init(struct dosvm *, bool locked);
void dosvm_pool_adopt(struct dosvm *); /* locked mode: frames of the eager loop */
int dosvm_pool_close(struct dosvm *);
/* Frame of a pool or EMS-window alias page. -ENOENT: neither (low page);
 * -EFAULT: unmapped window/outside quota; -EAGAIN: not resident and wait
 * false; -EIO/-ENOMEM: contained backing failure. write marks it dirty. */
int dosvm_pool_frame(struct dosvm *, uint32_t linear, bool write, bool wait, uint32_t *phys);
bool dosvm_pool_range(const struct dosvm *, uint32_t linear, bool *alias);
/* Cheap filter for hot paths: only the pool range and the EMS frame can be
 * pool pages; every other address keeps the plain translation path. */
static inline bool dosvm_pool_candidate(const struct vm_pool *pool, uint32_t linear)
{ return pool->pages && (linear >= VM_POOL_BASE || linear - pool->window_base < VM_POOL_WINDOWS * PAGE_SIZE); }
int dosvm_pool_fault(struct dosvm *, uint32_t linear, uint32_t error);
int dosvm_pool_prefault(struct dosvm *, uint32_t linear, uint32_t bytes, bool write);
int dosvm_pool_resident(struct dosvm *, uint32_t linear, uint32_t bytes); /* 0 or -EAGAIN */
void dosvm_pool_window(struct dosvm *, unsigned window_page, uint32_t pool_linear);
void dosvm_pool_zero(struct dosvm *, uint32_t linear, uint32_t pages);
void dosvm_pool_advise(struct dosvm *, uint32_t linear, uint32_t pages);
int dosvm_pool_pin(struct dosvm *, uint32_t linear, uint32_t pages);
void dosvm_pool_unpin(struct dosvm *, uint32_t linear, uint32_t pages);
void dosvm_pool_hold(struct dosvm *, bool enter);
void dosvm_pool_settle(struct dosvm *);
void dosvm_pool_irq_sample(struct dosvm *, uint64_t elapsed_ms);
void dosvm_memory_close(struct dosvm *);
void dosvm_page_remove(struct dosvm *, uint32_t);
bool dosvm_pool_alloc(struct dosvm *, struct vm_block *, uint32_t);
bool dosvm_pool_resize(struct dosvm *, struct vm_block *, uint32_t);
void dosvm_pool_free(struct dosvm *, struct vm_block *);
void dosvm_pool_query(struct dosvm *, uint32_t *, uint32_t *);
int dosvm_copy_region(struct dosvm *, uint32_t, uint32_t, uint32_t, bool);
int dosvm_xms(struct dosvm *, struct v86_frame *);
uint8_t dosvm_umb(struct dosvm *, struct v86_frame *, unsigned);
int dosvm_ems(struct dosvm *, struct v86_frame *);
int dosvm_ems_return(struct dosvm *, struct v86_frame *);
static inline uint16_t vm_u16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static inline uint32_t vm_u32(const uint8_t *p) { return vm_u16(p) | ((uint32_t)vm_u16(p+2) << 16); }
static inline void vm_w16(uint8_t *p, uint16_t x) { p[0]=x; p[1]=x>>8; }
static inline void vm_w32(uint8_t *p, uint32_t x) { vm_w16(p,x); vm_w16(p+2,x>>16); }
static inline void vm_low(uint32_t *p, uint16_t x) { *p=(*p&0xFFFF0000u)|x; }
static inline uint32_t vm_far(uint32_t x) { return (x>>16)*16+(uint16_t)x; }
static inline uint32_t vm_ptr(uint32_t seg, uint32_t off) { return (seg&0xFFFFu)*16+(off&0xFFFFu); }
/* Structures must not cross the caller's 64 KiB segment limit. */
static inline bool vm_buffer(struct dosvm *v, uint32_t seg, uint32_t off, unsigned n, bool wr)
{ return seg<=0xFFFF && n<=0x10000u-(uint16_t)off && dosvm_range(v,vm_ptr(seg,off),n,wr); }
#endif
