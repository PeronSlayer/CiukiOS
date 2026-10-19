/* Physical ownership and locked-buffer accounting. Units are 4 KiB pages.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_LEDGER_H
#define CIUKI_LEDGER_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

enum mm_owner {
    MM_RESERVED, MM_KERNEL_IMAGE, MM_KERNEL, MM_HEAP, MM_STACK,
    MM_TABLE, MM_DMA, MM_DEVICE, MM_CACHE_DATA, MM_CACHE_META,
    MM_CACHE_WORK, MM_USER_PRIVATE, MM_USER_SHARED, MM_VM_PRIVATE,
    MM_VM_POOL, MM_IO_REQUEST, MM_OWNERS
};
struct mm_zone_ledger {
    uint32_t usable, free, reserved, resident[MM_OWNERS];
};
struct mm_ledger {
    struct mm_zone_ledger zone[3];
    uint32_t reserved_va[MM_OWNERS], commit_ram[MM_OWNERS];
    uint32_t commit_pagefile, swap_capacity, swap_used;
    uint32_t mappings, shared_mappings, pin_tokens, pinned_pages, retired_pages;
    uint32_t low, high, pressure_crossings, pressure_suppressed;
    uint32_t mismatches, double_unpins, bad_context;
    bool pressure;
};
enum mm_direction { MM_PIN_CPU, MM_PIN_TO_DEVICE, MM_PIN_FROM_DEVICE, MM_PIN_BIDIRECTIONAL };
struct mm_pin {
    uint32_t generation, first_pfn, pages;
    uint32_t address_mask, alignment, boundary;
    enum mm_direction direction;
    /* Optional immutable PTE vector for an already-created eager VM pool.
     * It must remain resident and unchanged until unpin; pages <= 1024.
     * first_pfn is still the first data PFN. The table is also held. */
    uint32_t pfn_table, first_entry;
    /* Registry identity prevents copied/stale tokens from completing a new pin. */
    uint32_t ticket;
};

/* PMM calls these with interrupts excluded; no allocation, sleep or logging. */
void ledger_init(void);
void ledger_boot_page(uint32_t pfn, enum mm_owner owner, bool free);
void ledger_boot_done(void);
void ledger_alloc_page(uint32_t pfn);
void ledger_free_page(uint32_t pfn);
/* Ownership follows the allocating object, not each alias. The caller knows
 * its old category; transfer and free are performed under the PMM lock. */
void ledger_move(uint32_t phys, uint32_t pages, enum mm_owner from, enum mm_owner to) __attribute__((weak));
void ledger_promise(enum mm_owner, int32_t reserved, int32_t committed) __attribute__((weak));
void ledger_mapping(int32_t mappings, int32_t shared) __attribute__((weak));
void ledger_snapshot(struct mm_ledger *);
bool ledger_reconcile(void);
bool ledger_pressure(void);
void ledger_record(const char *profile) __attribute__((weak));
void ledger_poll(uint64_t now_ms);
void ledger_lifecycle(const char *, size_t) __attribute__((weak));
void ledger_audio_active(bool active) __attribute__((weak));
/* Critical work may use only already-resident storage. Pass true before an
 * operation that can allocate reclaimable backing or fault, plus its caller's
 * return address (or fault EIP). Count and rate-limit protected diagnostics;
 * only CIUKI_LEDGER_STRICT makes violations fatal, independently of NDEBUG. */
void ledger_assert_pinned_context(bool may_fault, uintptr_t site) __attribute__((weak));
void ledger_critical(bool enter) __attribute__((weak));
void ledger_irq_context(bool enter) __attribute__((weak));

/* Pins cover whole allocated PFNs, validate constraints in bytes, and never
 * change their physical owner. A retirement transfers the entire range to
 * the token: completion (or proven device stop) performs the final frees.
 * No caller may free a pinned range directly. Tokens are copied into the
 * registry; snapshots never chase client storage. */
int mm_pin(struct mm_pin *) __attribute__((weak));
int mm_unpin(struct mm_pin *, uint32_t generation) __attribute__((weak));
int mm_pin_retire(struct mm_pin *, uint32_t generation, enum mm_owner) __attribute__((weak));
bool ledger_page_pinned(uint32_t pfn);

/* Accounting wrappers keep old standalone host fixtures usable. Production
 * always links ledger.c; omission is only for fixtures with a fake PMM. */
static inline void mm_claim(uint32_t phys, uint32_t pages, enum mm_owner owner)
{ if (phys && ledger_move) ledger_move(phys, pages, MM_KERNEL, owner); }
static inline void mm_release(uint32_t phys, uint32_t pages, enum mm_owner owner)
{ if (phys && ledger_move) ledger_move(phys, pages, owner, MM_KERNEL); }
#endif
