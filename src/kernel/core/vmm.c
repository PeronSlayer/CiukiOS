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
/* Only the PFN descriptor reservation is borrowed before the full RAM map.
 * 4.5 MiB at an arbitrary 4 MiB offset needs at most three page tables. */
static uint32_t descriptor_boot_pt[3][1024] __attribute__((aligned(4096)));
void vmm_bootstrap_ram(uint32_t phys, uint32_t bytes)
{
    uint32_t *pd=P2V(read_cr3());
    uint32_t first=phys>>22, last=(phys+bytes-1)>>22;
    for (uint32_t i=first;i<=last;i++) {
        uint32_t *pt=descriptor_boot_pt[i-first];
        for (uint32_t j=0;j<1024;j++) {
            uint32_t p=(i<<22)+(j<<12);
            pt[j]=p>=phys && p-phys<bytes ? p|PTE_P|PTE_W : 0;
        }
        pd[768+i]=V2P(pt)|PTE_P|PTE_W;
    }
    write_cr3(read_cr3());
}
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

bool vmm_cache_alias_ok(uint32_t physical,uint32_t bytes,uint32_t flags)
{
    if (!bytes || (physical&4095) || (bytes&4095) || bytes-1>UINT32_MAX-physical) return false;
    uint64_t end=(uint64_t)physical+bytes;
    for (unsigned i=768;i<1024;i++) if (kpd[i]&PTE_P) {
        uint32_t *pt=table_va(kpd[i]&~4095u);
        for (unsigned j=0;j<1024;j++) {
            uint32_t p=pt[j], frame=p&~4095u;
            if ((p&PTE_P) && frame>=physical && frame<end &&
                (p&(PTE_PCD|PTE_PWT))!=(flags&(PTE_PCD|PTE_PWT))) return false;
            if (!((j+1)&15)) kpreempt_point();
        }
    }
    return true;
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
    /* Every address space copies PDEs 768-1022 at creation (as_create), and
     * they never change afterwards. The CPU sets the accessed flag of a PDE
     * in the directory it walks (Intel SDM Vol. 3A 4.8; the dirty flag only
     * in leaf entries), so a first kernel touch of a 4 MiB region would set
     * it in one copy only. Preset it, as Linux's _KERNPG_TABLE does: kernel
     * PDEs then stay identical in every directory (f6-21b). */
    for (unsigned i = 768; i < 1023; i++) {
        uint32_t pt = pmm_alloc_zone(ZONE_DMA);
        if (!pt)
            panic("vmm: no page for kernel table %u", i);
        mm_claim(pt, 1, MM_TABLE);
        memset(table_va(pt), 0, PAGE_SIZE);
        kpd[i] = pt | PTE_P | PTE_W | PTE_A;
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

/* Global alias sampling is incremental even for sparse spaces. The cursor
 * is repaired before teardown can free an address-space owner. No user PTEs
 * are global; inactive spaces flush on their next CR3 load (UP kernel). */
#define CIUKI_VMM_SAMPLER 1
static struct aspace *sample_spaces, *sample_space;
static uint32_t sample_va;
void vmm_sample_reset(void)
{
    uint32_t f=irq_save(); sample_space=sample_spaces; sample_va=0; irq_restore(f);
}
void vmm_sample_register(struct aspace *as)
{
    uint32_t f=irq_save();
    as->sample_next=sample_spaces; as->sample_prev=0;
    if (sample_spaces) sample_spaces->sample_prev=as;
    sample_spaces=as; irq_restore(f);
}
void vmm_sample_unregister(struct aspace *as)
{
    uint32_t f=irq_save();
    if (sample_space==as) { sample_space=as->sample_next; sample_va=0; }
    if (as->sample_prev) as->sample_prev->sample_next=as->sample_next;
    else if (sample_spaces==as) sample_spaces=as->sample_next;
    if (as->sample_next) as->sample_next->sample_prev=as->sample_prev;
    as->sample_prev=as->sample_next=0;
    irq_restore(f);
}
/* f3-46: replacement history is sampled (and A/D cleared) only for frames
 * of the replaceable owners, and never for a pinned frame. Device, kernel
 * and VM-private translations keep their bits for their owners: the DOS
 * VM's VGA window and pinned VBE buffer harvest by PTE dirty bits. */
static bool sample_owner(uint32_t pte)
{
    struct mm_page_descriptor *d=pmm_descriptor?pmm_descriptor(pte&~4095u):0;
    if (!d || d->pins) return false;
    unsigned owner=d->reserved&MM_PAGE_OWNER_MASK;
    return owner==MM_USER_PRIVATE || owner==MM_USER_SHARED || owner==MM_VM_POOL;
}
uint32_t as_sample_page(struct aspace *as, uint32_t va)
{
    uint32_t f=irq_save(), value=0;
    if (as->pd_phys && va<KERNEL_VBASE) {
        uint32_t de=table_va(as->pd_phys)[va>>22];
        if (de&PTE_P) {
            uint32_t *p=&table_va(de&~4095u)[(va>>12)&1023];
            value=*p;
            if ((value&(PTE_A|PTE_D)) && !sample_owner(value)) value&=~(PTE_A|PTE_D);
            else if (value&(PTE_A|PTE_D)) {
                *p=value&~(PTE_A|PTE_D);
                if (read_cr3()==as->pd_phys) invlpg(va);
            }
        }
    }
    irq_restore(f); return value;
}
bool vmm_sample_step(uint32_t epoch)
{
    uint32_t f=irq_save();
    if (!sample_space) { sample_space=sample_spaces; sample_va=0; }
    if (!sample_space) { irq_restore(f); return true; }
    struct aspace *as=sample_space;
    uint32_t de=table_va(as->pd_phys)[sample_va>>22];
    if (de&PTE_P) {
        uint32_t old=as_sample_page(as,sample_va);
        /* PTE_OWN also retains PROT_NONE translations. They cannot count as
         * physical references twice and a cleared bit never authorizes reuse. */
        if (old&(PTE_P|0x200u)) {
            struct mm_page_descriptor *d=pmm_descriptor?pmm_descriptor(old&~4095u):0;
            if (d && (d->reserved&MM_PAGE_OWNER_MASK)!=MM_RESERVED) {
                if (old&PTE_A) { d->accessed_epoch=epoch; d->reserved|=MM_PAGE_TOUCHED; }
                if (old&PTE_D) d->dirty_epoch=d->dirty_epoch==UINT32_MAX?1:d->dirty_epoch+1;
            }
        }
        sample_va+=PAGE_SIZE;
    } else sample_va=(sample_va&~0x3fffffu)+0x400000u;
    bool done=false;
    if (sample_va>=KERNEL_VBASE) {
        sample_space=as->sample_next; sample_va=0; done=!sample_space;
    }
    irq_restore(f); return done;
}

/* ---- user address spaces ---- */

/* O(1) registry mutation; fixtures that extract just address-space routines
 * deliberately omit the global sampler. */
#ifndef CIUKI_VMM_SAMPLER
void vmm_sample_register(struct aspace *) __attribute__((weak));
void vmm_sample_unregister(struct aspace *) __attribute__((weak));
#endif

int as_create(struct aspace *as)
{
#ifndef CIUKI_VMM_SAMPLER
    extern void vmm_sample_register(struct aspace *) __attribute__((weak));
#endif
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
    as->memory = 0;
    as->owner = MM_KERNEL; as->pool_base = 0;
    as->pages = 0;
    as->tables = 0;
    as->sample_prev=as->sample_next=0;
    if (&vmm_sample_register) vmm_sample_register(as);
    return 0;
}

static int as_map_internal(struct aspace *as, uint32_t va, uint32_t phys,
                           uint32_t flags, bool borrowed)
{
    if (!as->pd_phys || va >= KERNEL_VBASE || (borrowed &&
        (as->memory || (va & 4095) || (phys & 4095))))
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
    if ((*pte & PTE_BORROWED) && !as->memory) return -EINVAL;
    if (borrowed && *pte) return -EINVAL;
    if (!(*pte & PTE_P)) {
        as->pages++;
        g_user_mappings++;
        if (ledger_mapping) ledger_mapping(1, 0);
    }
    *pte = (phys & ~0xFFFu) | (flags & (PTE_W | PTE_U)) | PTE_P |
        (borrowed ? PTE_BORROWED | (flags & (PTE_PCD | PTE_PWT)) : 0);
    if (read_cr3() == as->pd_phys)
        invlpg(va);
    return 0;
}

int as_map(struct aspace *as, uint32_t va, uint32_t phys, uint32_t flags)
{ return as_map_internal(as, va, phys, flags, false); }

int as_map_borrowed(struct aspace *as, uint32_t va, uint32_t phys, uint32_t flags)
{ return as_map_internal(as, va, phys, flags, true); }

int as_seal_borrowed(struct aspace *as,uint32_t va,uint32_t phys)
{
    if (!as->pd_phys || as->memory || va>=KERNEL_VBASE || (va&4095) || (phys&4095)) return -EINVAL;
    uint32_t de=table_va(as->pd_phys)[va>>22];
    if (!(de&PTE_P)) return -EFAULT;
    uint32_t *p=&table_va(de&~4095u)[(va>>12)&1023];
    if ((*p&(PTE_P|PTE_BORROWED))!=(PTE_P|PTE_BORROWED) || (*p&~4095u)!=phys) return -EFAULT;
    *p &= ~PTE_W;
    if (read_cr3()==as->pd_phys) invlpg(va);
    return 0;
}

int as_revoke_borrowed(struct aspace *as, uint32_t va, uint32_t phys)
{
    if (!as->pd_phys || as->memory || va>=KERNEL_VBASE || (va&4095) || (phys&4095)) return -EINVAL;
    uint32_t de=table_va(as->pd_phys)[va>>22];
    if (!(de&PTE_P)) return -EFAULT;
    uint32_t *p=&table_va(de&~4095u)[(va>>12)&1023];
    if ((*p & (PTE_P|PTE_BORROWED))!=(PTE_P|PTE_BORROWED) || (*p&~4095u)!=phys) return -EFAULT;
    *p=0; as->pages--; g_user_mappings--;
    if (ledger_mapping) ledger_mapping(-1,0);
    if (read_cr3()==as->pd_phys) invlpg(va);
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
        if (!(pt[i] & PTE_P) || (!as->memory && (pt[i]&PTE_BORROWED)) ||
            (frames && (frames[i] & (PAGE_SIZE - 1)))) return -EFAULT;
    uint32_t d = 0, a = 0, stale = 0;
    for (unsigned i = 0; i < count; i++) {
        uint32_t old = pt[i];
        uint32_t next = frames ? frames[i] | (flags & (PTE_U | PTE_W)) | PTE_P : old & ~0x40u;
        if (old != next) {
            /* fx-doswin3: an entry read with A (or D) set is invalidated
             * whatever happens next; D implies A for a translation the
             * processor set it through. One read with both clear may still
             * meet a page walk (speculative ones included) before the
             * store, so that store is one locked exchange and its result
             * decides (Intel SDM Vol. 3A rev. 090, 11.1.2.1 "Automatic
             * Locking": XCHG with a memory operand). A later walk loads
             * the new entry. */
            if (old & 0x60u) { pt[i] = next; stale |= 1u << i; }
            else {
                old = __atomic_exchange_n(&pt[i], next, __ATOMIC_SEQ_CST);
                if (old & 0x60u) stale |= 1u << i;
            }
        }
        if (old & 0x40u) d |= 1u << i;
        if (old & 0x20u) a |= 1u << i;
    }
    /* fx-27: invalidate exactly the changed entries (at most 32, below
     * Linux's 33-page tlb_single_page_flush_ceiling). The kernel's own
     * mappings are not global (no CR4.PGE), so a CR3 reload would also
     * discard every kernel and guest translation the rest of the exit and
     * the guest's next instructions need (Intel SDM Vol. 3A 4.10.4.1).
     * INVLPG of an entry whose D was cleared makes the CPU set D again on
     * the next native store, so the next collection still sees it.
     * fx-doswin3: only a changed entry whose replaced value had A (or D) set.
     * "The processor does not cache a translation for a page number unless
     * the accessed flag is 1 in each of the paging-structure entries used
     * during translation" (Intel SDM Vol. 3A rev. 090, 5.10.2.3 "Details of
     * TLB Use"; this includes speculative and prefetch translations), and
     * every path that clears A in a present entry invalidates it while its
     * space is current (this function, as_sample_page, dosvm_pool
     * pte_sample; a CR3 load flushes the others: no CR4.PGE), so an entry
     * replaced with A clear has no TLB entry to invalidate. FreeBSD's i386
     * pmap_enter applies the same rule (sys/i386/i386/pmap.c: invalidate
     * only when the old PTE had PG_A). INVLPG is a serializing instruction
     * (SDM Vol. 3A, "Serializing Instructions"). A Mode Y plane switch
     * touches about four of the window's sixteen pages per column (one
     * page holds 51 rows of a plane), so most of each switch's sixteen
     * INVLPGs were redundant. */
    if (stale && read_cr3() == as->pd_phys)
        for (unsigned i = 0; i < count; i++) if (stale & (1u << i)) invlpg(va + i * PAGE_SIZE);
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
#ifndef CIUKI_VMM_SAMPLER
    extern void vmm_sample_unregister(struct aspace *) __attribute__((weak));
#endif
    if (!as->pd_phys)
        return;
    if (&vmm_sample_unregister) vmm_sample_unregister(as);
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
                    bool borrowed=!as->memory && (old&PTE_BORROWED);
                    if (!borrowed) mm_free(old & ~0xFFFu, as->pool_base && (i * 1024u + j) * PAGE_SIZE >= as->pool_base ? MM_VM_POOL : as->owner);
                    if (!borrowed && as->owner==MM_USER_PRIVATE && ledger_promise) ledger_promise(MM_USER_PRIVATE,0,-1);
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
