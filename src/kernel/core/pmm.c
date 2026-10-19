/* Physical page allocator: one bit per page below 768 MiB, zones LOW/DMA/
 * NORMAL. Only normalized E820 type-1 RAM is ever handed out; the first MiB
 * stays out of the general allocator (docs/design/boot-memory.md).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/mm.h>
#include <ciuki/cpu.h>
#include <ciuki/ledger.h>

extern char __kernel_start[], __kernel_end[];

static uint32_t bitmap[PMM_MAX_PAGES / 32];     /* 1 = not free */
static uint32_t zone_total[3], zone_free[3];
static uint32_t hint[3];

#define MAX_RES 16
static struct pmm_reservation res[MAX_RES];
static unsigned nres;

static int zone_of(uint32_t pfn)
{
    if (pfn < 256)
        return ZONE_LOW;
    if (pfn < 4096)
        return ZONE_DMA;
    return ZONE_NORMAL;
}

static bool used(uint32_t pfn) { return bitmap[pfn >> 5] & (1u << (pfn & 31)); }
static void set_used(uint32_t pfn) { bitmap[pfn >> 5] |= 1u << (pfn & 31); }
static void set_free(uint32_t pfn) { bitmap[pfn >> 5] &= ~(1u << (pfn & 31)); }

static void add_res(uint32_t start, uint32_t end, const char *why)
{
    if (nres < MAX_RES && end > start)
        res[nres++] = (struct pmm_reservation){ start, end, why };
}

static bool in_ram(uint32_t phys)
{
    bool ram=false;
    for (uint32_t i = 0; i < g_boot.e820_count; i++) {
        const struct ciuki_e820 *e = &g_boot.e820[i];
        if (e->type != CBI_E820_RAM) {
            if ((uint64_t)phys < e->base+e->length && (uint64_t)phys+PAGE_SIZE > e->base) return false;
            continue;
        }
        uint64_t s = (e->base + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t t = (e->base + e->length) & ~(uint64_t)(PAGE_SIZE - 1);
        if (phys >= s && (uint64_t)phys + PAGE_SIZE <= t)
            ram=true;
    }
    return ram;
}

bool pmm_is_reserved(uint32_t phys)
{
    if (phys < 0x100000u || !in_ram(phys))
        return true;
    for (unsigned i = 0; i < nres; i++)
        if (phys + PAGE_SIZE > res[i].start && phys < res[i].end)
            return true;
    return false;
}

unsigned pmm_reservations(const struct pmm_reservation **out)
{
    *out = res;
    return nres;
}

void pmm_init(void)
{
    memset(bitmap, 0xFF, sizeof(bitmap));
    memset(zone_total, 0, sizeof(zone_total));
    ledger_init();
    nres = 0;
    add_res(0, 0x100000u, "first_mib_ivt_bda_ebda_rom");
    uint32_t ks = g_boot.kernel_start, ke = g_boot.kernel_end;
    uint32_t ls = V2P(__kernel_start), le = V2P(__kernel_end);
    add_res(ks < ls ? ks : ls, PAGE_ALIGN_UP(ke > le ? ke : le), "kernel_image");
    if (g_boot.fb_phys && !(g_boot.flags & CBI_F_TEXT_MODE))
        add_res(g_boot.fb_phys, g_boot.fb_phys + g_boot.fb_pitch * g_boot.fb_height, "boot_framebuffer");

    for (uint32_t i = 0; i < g_boot.e820_count; i++) {
        const struct ciuki_e820 *e = &g_boot.e820[i];
        if (e->type != CBI_E820_RAM)
            continue;
        uint64_t s = (e->base + PAGE_SIZE - 1) >> 12;
        uint64_t t = (e->base + e->length) >> 12;
        if (t > PMM_MAX_PAGES)
            t = PMM_MAX_PAGES;
        for (uint64_t pfn = s < 256 ? 256 : s; pfn < t; pfn++)
            set_free((uint32_t)pfn);
    }
    /* Non-RAM descriptors win over overlapping RAM (loader already
     * normalized; this keeps the allocator safe regardless). */
    for (uint32_t i = 0; i < g_boot.e820_count; i++) {
        const struct ciuki_e820 *e = &g_boot.e820[i];
        if (e->type == CBI_E820_RAM)
            continue;
        uint64_t s = e->base >> 12, t = (e->base + e->length + PAGE_SIZE - 1) >> 12;
        for (uint64_t pfn = s; pfn < t && pfn < PMM_MAX_PAGES; pfn++)
            set_used((uint32_t)pfn);
    }
    for (unsigned i = 0; i < nres; i++)
        for (uint32_t pfn = res[i].start >> 12; pfn < (res[i].end + PAGE_SIZE - 1) >> 12 && pfn < PMM_MAX_PAGES; pfn++)
            set_used(pfn);

    for (int z = 0; z < 3; z++)
        zone_free[z] = 0;
    for (uint32_t pfn = 0; pfn < PMM_MAX_PAGES; pfn++)
        if (in_ram(pfn << 12)) {
            zone_total[zone_of(pfn)]++;
            if (!used(pfn)) zone_free[zone_of(pfn)]++;
            enum mm_owner owner = MM_RESERVED;
            if ((pfn << 12) < res[1].end && (pfn << 12) + PAGE_SIZE > res[1].start)
                owner = MM_KERNEL_IMAGE;
            ledger_boot_page(pfn, owner, !used(pfn));
        }
    hint[ZONE_LOW] = 0;
    hint[ZONE_DMA] = 256;
    hint[ZONE_NORMAL] = 4096;
    ledger_boot_done();
    ledger_reconcile();
}

static uint32_t alloc_zone(int zone)
{
    static const uint32_t lo[3] = { 0, 256, 4096 }, hi[3] = { 256, 4096, PMM_MAX_PAGES };
    if (zone < ZONE_DMA || zone > ZONE_NORMAL || !zone_free[zone]) return 0;
    for (unsigned pass = 0; pass < 2; pass++) {
        uint32_t start = pass ? lo[zone] : hint[zone];
        for (uint32_t pfn = start; pfn < hi[zone];) {
            uint32_t f = irq_save();
            uint32_t end = (pfn + 32) & ~31u;
            if (end > hi[zone]) end = hi[zone];
            for (; pfn < end; pfn++) if (!used(pfn)) {
                set_used(pfn); zone_free[zone]--; hint[zone] = pfn + 1;
                ledger_alloc_page(pfn);
                irq_restore(f); return pfn << 12;
            }
            irq_restore(f);
        }
    }
    return 0;
}

uint32_t pmm_alloc_zone(int zone)
{
    if (ledger_assert_pinned_context) ledger_assert_pinned_context(true,(uintptr_t)__builtin_return_address(0));
    return alloc_zone(zone);
}

uint32_t pmm_alloc(void)
{
    if (ledger_assert_pinned_context) ledger_assert_pinned_context(true,(uintptr_t)__builtin_return_address(0));
    uint32_t p = alloc_zone(ZONE_NORMAL);
    return p ? p : alloc_zone(ZONE_DMA);
}

/* Physically contiguous run for mount-time parser workspaces (f4-04). NORMAL
 * zone first, then DMA; never LOW. The run is freed page by page with
 * pmm_free(). Returns the physical base or 0; the page allocator hint is
 * untouched, so ordinary single-page allocation behavior does not change. */
/* Tentative pages are owned by this invocation before IF reopens. IRQ
 * allocators cannot take them. On fragmentation roll back only our prefix,
 * in bounded chunks too; never scan/reserve a whole zone with IF clear. */
static uint32_t alloc_run(int zone, uint32_t pages, uint32_t alignment)
{
    uint32_t low = zone == ZONE_DMA ? 256 : 4096;
    uint32_t high = zone == ZONE_DMA ? 4096 : PMM_MAX_PAGES;
    if (pages > high - low || zone_free[zone] < pages) return 0;
    uint32_t start = (low + alignment - 1) & ~(alignment - 1);
    while (start <= high - pages) {
        uint32_t have = 0;
        bool blocked = false;
        while (have < pages && !blocked) {
            uint32_t f = irq_save();
            uint32_t end = pages - have > 32 ? have + 32 : pages;
            for (; have < end; have++) {
                if (used(start + have)) { blocked = true; break; }
                set_used(start + have); zone_free[zone]--;
                ledger_alloc_page(start + have);
            }
            irq_restore(f);
        }
        if (!blocked) return start << 12;
        uint32_t next = (start + have + alignment) & ~(alignment - 1);
        for (uint32_t i = 0; i < have;) {
            uint32_t f = irq_save();
            uint32_t end = have - i > 32 ? i + 32 : have;
            for (; i < end; i++) { set_free(start + i); zone_free[zone]++; ledger_free_page(start + i); }
            irq_restore(f);
        }
        start = next;
    }
    return 0;
}

uint32_t pmm_alloc_contig(uint32_t pages)
{
    if (ledger_assert_pinned_context) ledger_assert_pinned_context(true,(uintptr_t)__builtin_return_address(0));
    if (!pages) return 0;
    uint32_t p = alloc_run(ZONE_NORMAL, pages, 1);
    return p ? p : alloc_run(ZONE_DMA, pages, 1);
}

uint32_t pmm_alloc_dma_run(uint32_t pages, uint32_t alignment)
{
    if (ledger_assert_pinned_context) ledger_assert_pinned_context(true,(uintptr_t)__builtin_return_address(0));
    if (!pages || pages > 3840 || !alignment || alignment > 4096 || (alignment & (alignment - 1))) return 0;
    return alloc_run(ZONE_DMA, pages, alignment);
}

void pmm_free(uint32_t phys)
{
    uint32_t pfn = phys >> 12;
    if (pfn >= PMM_MAX_PAGES || pfn < 256 || pmm_is_reserved(phys))
        panic("pmm_free: invalid page %08x", phys);
    uint32_t f = irq_save();
    if (ledger_page_pinned(pfn)) {
        irq_restore(f);
        panic("pmm_free: pinned page %08x", phys);
    }
    if (!used(pfn)) {
        irq_restore(f);
        panic("pmm_free: double free %08x", phys);
    }
    set_free(pfn);
    ledger_free_page(pfn);
    zone_free[zone_of(pfn)]++;
    if (pfn < hint[zone_of(pfn)])
        hint[zone_of(pfn)] = pfn;
    irq_restore(f);
}

bool pmm_page_free(uint32_t pfn) { return pfn < PMM_MAX_PAGES && !used(pfn); }

void pmm_free_owned(uint32_t phys, enum mm_owner owner)
{
    uint32_t f = irq_save();
    mm_release(phys, 1, owner);
    pmm_free(phys);
    irq_restore(f);
}

uint32_t pmm_free_count(void) { return zone_free[ZONE_DMA] + zone_free[ZONE_NORMAL]; }
uint32_t pmm_zone_free(int z) { return zone_free[z]; }
uint32_t pmm_zone_total(int z) { return zone_total[z]; }
uint32_t pmm_total_usable(void) { return zone_total[0] + zone_total[1] + zone_total[2]; }
