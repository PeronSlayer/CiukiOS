/* Kernel page tables, direct map, MMIO and stack regions, user address
 * spaces (docs/design/boot-memory.md "Virtual layout").
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/mm.h>
#include <ciuki/cpu.h>

extern char __text_start[], __rodata_end[];

#define DIRECT_LIMIT   0x30000000u          /* 768 MiB direct map */
#define STACK_REGION   0xF0000000u
#define STACK_SLOT     (PAGE_SIZE + KSTACK_SIZE) /* guard + usable pages */
#define STACK_SLOTS    1360u
#define MMIO_REGION    0xF8000000u
#define MMIO_END       0xFF800000u

_Static_assert(KSTACK_SIZE % PAGE_SIZE == 0, "kernel stack must contain whole pages");
_Static_assert(STACK_REGION + STACK_SLOTS * STACK_SLOT <= MMIO_REGION, "kernel stack region");

static uint32_t kpd_phys;
static uint32_t *kpd;                        /* via direct map */
static uint32_t mmio_next = MMIO_REGION;
static uint8_t stack_slot_used[STACK_SLOTS];
uint32_t g_user_mappings;

static uint32_t *table_va(uint32_t phys) { return (uint32_t *)P2V(phys); }

static uint32_t *kpte_ptr(uint32_t va)
{
    uint32_t pde = kpd[va >> 22];
    if (!(pde & PTE_P))
        return 0;
    return &table_va(pde & ~0xFFFu)[(va >> 12) & 1023];
}

static void kmap(uint32_t va, uint32_t phys, uint32_t flags)
{
    uint32_t *pte = kpte_ptr(va);
    if (!pte)
        panic("kmap: no kernel table for %08x", va);
    *pte = (phys & ~0xFFFu) | flags | PTE_P;
}

static void kunmap(uint32_t va)
{
    uint32_t *pte = kpte_ptr(va);
    if (pte) {
        *pte = 0;
        invlpg(va);
    }
}

uint32_t vmm_kernel_pd(void) { return kpd_phys; }

uint32_t vmm_kernel_pte(uint32_t va)
{
    uint32_t *p = kpte_ptr(va);
    return p ? *p : 0;
}

void vmm_init(void)
{
    /* Tables must be reachable through the boot map (0-16 MiB). */
    kpd_phys = pmm_alloc_zone(ZONE_DMA);
    if (!kpd_phys)
        panic("vmm: no page for the kernel directory");
    kpd = table_va(kpd_phys);
    memset(kpd, 0, PAGE_SIZE);
    for (unsigned i = 768; i < 1023; i++) {
        uint32_t pt = pmm_alloc_zone(ZONE_DMA);
        if (!pt)
            panic("vmm: no page for kernel table %u", i);
        memset(table_va(pt), 0, PAGE_SIZE);
        kpd[i] = pt | PTE_P | PTE_W;
    }
    kpd[1023] = kpd_phys | PTE_P | PTE_W;   /* recursive window */

    /* First MiB: RAM below A0000, uncached VGA/ROM window above. */
    for (uint32_t p = 0; p < 0x100000u; p += PAGE_SIZE)
        kmap(KERNEL_VBASE + p, p, PTE_W | (p >= 0xA0000u ? (PTE_PCD | PTE_PWT) : 0));

    /* Direct map of RAM descriptors. */
    uint64_t fb_start = 0, fb_end = 0;
    if (g_boot.fb_phys && !(g_boot.flags & CBI_F_TEXT_MODE)) {
        fb_start = g_boot.fb_phys;
        fb_end = fb_start + (uint64_t)g_boot.fb_pitch * g_boot.fb_height;
    }
    for (uint32_t i = 0; i < g_boot.e820_count; i++) {
        const struct ciuki_e820 *e = &g_boot.e820[i];
        if (e->type != CBI_E820_RAM)
            continue;
        uint64_t s = (e->base + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t t = (e->base + e->length) & ~(uint64_t)(PAGE_SIZE - 1);
        if (s < 0x100000u)
            s = 0x100000u;
        if (t > DIRECT_LIMIT)
            t = DIRECT_LIMIT;
        for (uint64_t p = s; p < t; p += PAGE_SIZE) {
            /* Never alias the framebuffer with a cacheable mapping. */
            if (fb_end > fb_start && p + PAGE_SIZE > fb_start && p < fb_end)
                continue;
            kmap(KERNEL_VBASE + (uint32_t)p, (uint32_t)p, PTE_W);
        }
    }
    /* Kernel text and read-only data are read-only (CR0.WP is set). */
    for (uint32_t va = (uint32_t)__text_start; va < (uint32_t)__rodata_end; va += PAGE_SIZE)
        kmap(va, V2P(va), 0);

    write_cr3(kpd_phys);
}

void *vmm_map_mmio(uint32_t phys, uint32_t bytes, bool uncached)
{
    uint32_t off = phys & 0xFFFu;
    uint64_t span64 = ((uint64_t)bytes + off + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    /* The mapping plus its unmapped guard page must stay inside the region. */
    if (!bytes || (uint64_t)phys + bytes > 0x100000000ull || mmio_next >= MMIO_END ||
        span64 + PAGE_SIZE > MMIO_END - mmio_next)
        return 0;
    uint32_t span = (uint32_t)span64;
    uint32_t va = mmio_next;
    for (uint32_t i = 0; i < span; i += PAGE_SIZE)
        kmap(va + i, (phys & ~0xFFFu) + i, PTE_W | (uncached ? (PTE_PCD | PTE_PWT) : 0));
    mmio_next += span + PAGE_SIZE;            /* unmapped gap between mappings */
    return (void *)(va + off);
}

void *kstack_alloc(void)
{
    for (unsigned s = 0; s < STACK_SLOTS; s++) {
        if (stack_slot_used[s])
            continue;
        uint32_t base = STACK_REGION + s * STACK_SLOT;
        uint32_t pages[KSTACK_SIZE / PAGE_SIZE];
        for (unsigned i = 0; i < ARRAY_SIZE(pages); i++) {
            pages[i] = pmm_alloc();
            if (!pages[i]) {
                while (i) pmm_free(pages[--i]);
                return 0;
            }
        }
        stack_slot_used[s] = 1;
        kunmap(base);                         /* guard page stays unmapped */
        for (unsigned i = 0; i < ARRAY_SIZE(pages); i++) {
            uint32_t va = base + (i + 1) * PAGE_SIZE;
            kmap(va, pages[i], PTE_W);
            invlpg(va);
        }
        return (void *)(uintptr_t)(base + PAGE_SIZE);
    }
    return 0;
}

uint32_t kstack_guard_va(void *base) { return (uint32_t)(uintptr_t)base - PAGE_SIZE; }

void kstack_free(void *base)
{
    uint32_t b = (uint32_t)(uintptr_t)base - PAGE_SIZE;
    unsigned s = (b - STACK_REGION) / STACK_SLOT;
    if (b < STACK_REGION || s >= STACK_SLOTS || !stack_slot_used[s])
        panic("kstack_free: bad stack %p", base);
    for (unsigned i = 1; i <= KSTACK_SIZE / PAGE_SIZE; i++) {
        uint32_t pte = vmm_kernel_pte(b + i * PAGE_SIZE);
        kunmap(b + i * PAGE_SIZE);
        pmm_free(pte & ~0xFFFu);
    }
    stack_slot_used[s] = 0;
}

/* ---- user address spaces ---- */

int as_create(struct aspace *as)
{
    uint32_t pd = pmm_alloc();
    if (!pd)
        return -ENOMEM;
    uint32_t *v = table_va(pd);
    memset(v, 0, PAGE_SIZE);
    for (unsigned i = 768; i < 1023; i++)
        v[i] = kpd[i];
    v[1023] = pd | PTE_P | PTE_W;
    as->pd_phys = pd;
    as->pages = 0;
    as->tables = 0;
    return 0;
}

int as_map(struct aspace *as, uint32_t va, uint32_t phys, uint32_t flags)
{
    if (va >= KERNEL_VBASE)
        return -EINVAL;
    uint32_t *pd = table_va(as->pd_phys);
    uint32_t *pde = &pd[va >> 22];
    if (!(*pde & PTE_P)) {
        uint32_t pt = pmm_alloc();
        if (!pt)
            return -ENOMEM;
        memset(table_va(pt), 0, PAGE_SIZE);
        *pde = pt | PTE_P | PTE_W | PTE_U;
        as->tables++;
    }
    uint32_t *pte = &table_va(*pde & ~0xFFFu)[(va >> 12) & 1023];
    if (!(*pte & PTE_P)) {
        as->pages++;
        g_user_mappings++;
    }
    *pte = (phys & ~0xFFFu) | (flags & (PTE_W | PTE_U)) | PTE_P;
    if (read_cr3() == as->pd_phys)
        invlpg(va);
    return 0;
}

uint32_t as_lookup(const struct aspace *as, uint32_t va, uint32_t *pte_out)
{
    if (va >= KERNEL_VBASE)
        return 0;
    uint32_t pde = table_va(as->pd_phys)[va >> 22];
    if (!(pde & PTE_P))
        return 0;
    uint32_t pte = table_va(pde & ~0xFFFu)[(va >> 12) & 1023];
    if (pte_out)
        *pte_out = pte;
    return (pte & PTE_P) ? ((pte & ~0xFFFu) | (va & 0xFFFu)) : 0;
}

void as_destroy(struct aspace *as)
{
    if (!as->pd_phys)
        return;
    if (read_cr3() == as->pd_phys)
        write_cr3(kpd_phys);
    uint32_t *pd = table_va(as->pd_phys);
    for (unsigned i = 0; i < 768; i++) {
        if (!(pd[i] & PTE_P))
            continue;
        uint32_t *pt = table_va(pd[i] & ~0xFFFu);
        for (unsigned j = 0; j < 1024; j++)
            if (pt[j] & PTE_P) {
                pmm_free(pt[j] & ~0xFFFu);
                g_user_mappings--;
            }
        pmm_free(pd[i] & ~0xFFFu);
    }
    pmm_free(as->pd_phys);
    as->pd_phys = 0;
    as->pages = as->tables = 0;
}

bool as_range_ok(const struct aspace *as, uint32_t va, uint32_t len, bool write)
{
    if (len == 0)
        return true;
    if (va + len < va || va + len > KERNEL_VBASE)
        return false;
    for (uint32_t p = va & ~0xFFFu; p < va + len; p += PAGE_SIZE) {
        uint32_t pte = 0;
        if (!as_lookup(as, p, &pte))
            return false;
        if (!(pte & PTE_U) || (write && !(pte & PTE_W)))
            return false;
        if (p + PAGE_SIZE < p)
            break;
    }
    return true;
}
