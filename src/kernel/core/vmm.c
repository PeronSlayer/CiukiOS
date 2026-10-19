/* Kernel page tables, direct map, MMIO and stack regions, user address
 * spaces (docs/design/boot-memory.md "Virtual layout").
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/mm.h>
#include <ciuki/cpu.h>
#include <ciuki/mtrr.h>
#include <ciuki/task.h>

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
    mm_claim(kpd_phys, 1, MM_TABLE);
    kpd = table_va(kpd_phys);
    memset(kpd, 0, PAGE_SIZE);
    for (unsigned i = 768; i < 1023; i++) {
        uint32_t pt = pmm_alloc_zone(ZONE_DMA);
        if (!pt)
            panic("vmm: no page for kernel table %u", i);
        mm_claim(pt, 1, MM_TABLE);
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

static void *map_physical(uint32_t phys, uint32_t bytes, uint32_t flags)
{
    uint32_t off = phys & 0xFFFu;
    uint64_t span64 = ((uint64_t)bytes + off + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    /* The mapping plus its unmapped guard page must stay inside the region. */
    if (!bytes || (uint64_t)phys + bytes > 0x100000000ull || mmio_next >= MMIO_END ||
        span64 + PAGE_SIZE > MMIO_END - mmio_next)
        return 0;
    uint32_t span = (uint32_t)span64;
    uint32_t va = mmio_next;
    /* Reserve the private window before yielding so another mapper cannot
     * borrow it. The caller publishes the device pointer only on return.
     * 128 completed PTE updates * 1 us =128 us per preparation slice. */
    mmio_next += span + PAGE_SIZE;
    for (uint32_t i = 0; i < span; i += PAGE_SIZE) {
        kmap(va + i, (phys & ~0xFFFu) + i, flags);
        if (!((i / PAGE_SIZE + 1) & 127u)) kpreempt_point();
    }
    return (void *)(uintptr_t)(va + off);
}

void *vmm_map_mmio(uint32_t phys, uint32_t bytes, bool uncached)
{ return map_physical(phys, bytes, PTE_W | (uncached ? (PTE_PCD | PTE_PWT) : 0)); }

const void *vmm_map_physical_ro(uint32_t phys, uint32_t bytes)
{ return map_physical(phys, bytes, 0); }

void *vmm_map_framebuffer(uint32_t phys, uint32_t bytes)
{
    uint64_t span = ((uint64_t)bytes + (phys & 0xFFFu) + PAGE_SIZE - 1) &
                    ~(uint64_t)(PAGE_SIZE - 1);
    /* Check capacity before changing cache policy. This path runs first from
     * the early console; the direct map excludes the LFB, so no alias exists. */
    if (!bytes || (g_boot.flags & CBI_F_TEXT_MODE) || !phys || phys != g_boot.fb_phys ||
        (uint64_t)g_boot.fb_pitch * g_boot.fb_height != bytes ||
        (uint64_t)phys + bytes > 0x100000000ull || mmio_next >= MMIO_END ||
        span + PAGE_SIZE > MMIO_END - mmio_next) return 0;
    bool wc = mtrr_framebuffer_enable(phys, bytes);
    /* PCD=PWT=PAT=0: verified PAT[0]=WB, combined with MTRR WC gives WC.
     * Failure keeps PCD=PWT=1 (PAT[3]=UC). No PAT MSR is rewritten. */
    return vmm_map_mmio(phys, bytes, !wc);
}

void *vmm_reserve_mmio(uint32_t capacity)
{
    uint64_t span = ((uint64_t)capacity + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (!capacity || mmio_next >= MMIO_END || span + PAGE_SIZE > MMIO_END - mmio_next) return 0;
    uint32_t va = mmio_next;
    mmio_next += (uint32_t)span + PAGE_SIZE;
    return (void *)(uintptr_t)va;
}

void vmm_revoke_mmio(void *mapped, uint32_t bytes)
{
    uint32_t va = (uint32_t)(uintptr_t)mapped, start = PAGE_ALIGN_DOWN(va);
    uint64_t end = ((uint64_t)va + bytes + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (!bytes || start < MMIO_REGION || end > MMIO_END) return;
    for (uint32_t p = start; p < end; p += PAGE_SIZE) kunmap(p);
}

static void *replace_mmio(void *window, uint32_t capacity, uint32_t phys, uint32_t bytes, bool framebuffer)
{
    uint32_t va = (uint32_t)(uintptr_t)window, off = phys & (PAGE_SIZE - 1);
    uint64_t span = ((uint64_t)bytes + off + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (!bytes || span > capacity || va < MMIO_REGION || (va & (PAGE_SIZE - 1)) ||
        (uint64_t)va + capacity > MMIO_END || (uint64_t)phys + bytes > 0x100000000ull) return 0;
    vmm_revoke_mmio(window, capacity);
    bool wc = framebuffer && mtrr_framebuffer_reconfigure(phys, bytes);
    for (uint32_t i = 0; i < span; i += PAGE_SIZE) {
        kmap(va + i, PAGE_ALIGN_DOWN(phys) + i, PTE_W | (wc ? 0 : PTE_PCD | PTE_PWT));
        invlpg(va + i);
    }
    return (void *)(uintptr_t)(va + off);
}

void *vmm_replace_mmio(void *window, uint32_t capacity, uint32_t phys, uint32_t bytes)
{ return replace_mmio(window, capacity, phys, bytes, false); }

void *vmm_replace_framebuffer(void *window, uint32_t capacity, uint32_t phys, uint32_t bytes)
{ return replace_mmio(window, capacity, phys, bytes, true); }

void *kstack_alloc(void)
{
    for (unsigned s = 0; s < STACK_SLOTS; s++) {
        if (stack_slot_used[s])
            continue;
        uint32_t base = STACK_REGION + s * STACK_SLOT;
        uint32_t pages[KSTACK_SIZE / PAGE_SIZE];
        for (unsigned i = 0; i < ARRAY_SIZE(pages); i++) {
            pages[i] = pmm_alloc();
            mm_claim(pages[i], 1, MM_STACK);
            if (!pages[i]) {
                while (i) mm_free(pages[--i], MM_STACK);
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
        mm_free(pte & ~0xFFFu, MM_STACK);
    }
    stack_slot_used[s] = 0;
}

/* ---- user address spaces ---- */

int as_create(struct aspace *as)
{
    uint32_t pd = pmm_alloc();
    if (!pd)
        return -ENOMEM;
    mm_claim(pd, 1, MM_TABLE);
    uint32_t *v = table_va(pd);
    memset(v, 0, PAGE_SIZE);
    for (unsigned i = 768; i < 1023; i++)
        v[i] = kpd[i];
    v[1023] = pd | PTE_P | PTE_W;
    as->pd_phys = pd;
    as->owner = MM_KERNEL; as->pool_base = 0;
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
        mm_claim(pt, 1, MM_TABLE);
        memset(table_va(pt), 0, PAGE_SIZE);
        *pde = pt | PTE_P | PTE_W | PTE_U;
        as->tables++;
    }
    uint32_t *pte = &table_va(*pde & ~0xFFFu)[(va >> 12) & 1023];
    if (!(*pte & PTE_P)) {
        as->pages++;
        g_user_mappings++;
        if (ledger_mapping) ledger_mapping(1, 0);
    }
    *pte = (phys & ~0xFFFu) | (flags & (PTE_W | PTE_U)) | PTE_P;
    if (read_cr3() == as->pd_phys)
        invlpg(va);
    return 0;
}

int as_replace_pages(struct aspace *as, uint32_t va, const uint32_t *frames,
                     unsigned count, uint32_t flags, uint32_t *dirty, uint32_t *accessed)
{
    if (!as->pd_phys || !count || count > 32 || (va & (PAGE_SIZE - 1)) ||
        va >= KERNEL_VBASE || count > 1024u - ((va >> 12) & 1023u)) return -EINVAL;
    uint32_t pde = table_va(as->pd_phys)[va >> 22];
    if (!(pde & PTE_P)) return -EFAULT;
    volatile uint32_t *pt = &table_va(pde & ~0xFFFu)[(va >> 12) & 1023];
    /* Validate the whole span before changing any entry. */
    for (unsigned i = 0; i < count; i++)
        if (!(pt[i] & PTE_P) || (frames && (frames[i] & (PAGE_SIZE - 1)))) return -EFAULT;
    uint32_t d = 0, a = 0;
    bool changed = false;
    for (unsigned i = 0; i < count; i++) {
        uint32_t old = pt[i];
        if (old & 0x40u) d |= 1u << i;
        if (old & 0x20u) a |= 1u << i;
        uint32_t next = frames ? frames[i] | (flags & (PTE_U | PTE_W)) | PTE_P : old & ~0x40u;
        if (old != next) { pt[i] = next; changed = true; }
    }
    /* No global user PTEs. CR3 reload also invalidates cleared D bits, so
     * subsequent native stores are visible to the next collection. */
    if (changed && read_cr3() == as->pd_phys) write_cr3(as->pd_phys);
    if (dirty) *dirty = d;
    if (accessed) *accessed = a;
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
    /* 16 * (6 us release + 1 us PTE check) = 112 us. Count absent
     * entries too: a sparse address space must not evade the batch bound.
     * The owner is quiescent; clear ownership before any service point. */
    for (unsigned i = 0; i < 768; i++) {
        if (pd[i] & PTE_P) {
            uint32_t *pt = table_va(pd[i] & ~0xFFFu);
            for (unsigned j = 0; j < 1024; j++) {
                uint32_t old = pt[j];
                if (old & PTE_P) {
                    pt[j] = 0;
                    mm_free(old & ~0xFFFu, as->pool_base && (i * 1024u + j) * PAGE_SIZE >= as->pool_base ? MM_VM_POOL : as->owner);
                    if (as->owner==MM_USER_PRIVATE && ledger_promise) ledger_promise(MM_USER_PRIVATE,0,-1);
                    as->pages--; g_user_mappings--;
                    if (ledger_mapping) ledger_mapping(-1, 0);
                }
                if (!((j + 1) & 15u)) kpreempt_point();
            }
            uint32_t table = pd[i] & ~0xFFFu;
            pd[i] = 0; as->tables--;
            mm_free(table, MM_TABLE);
        }
        if (!((i + 1) & 15u)) kpreempt_point();
    }
    mm_free(as->pd_phys, MM_TABLE);
    as->pd_phys = 0;
    as->pages = as->tables = 0;
}

/* Guest validation calls this for at most one page inside an indivisible
 * emulation transaction. Legacy syscall buffers are capped at their public
 * limits; keep their validation atomic too (see F0-44 caller inventory). */
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
