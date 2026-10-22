/* f6-21b: kernel PDEs 768-1022 stay identical in every address space.
 * Production vmm_init()/as_create() on synthetic RAM. The CPU's page walk
 * is modelled per Intel SDM Vol. 3A 4.8: every paging-structure entry used
 * for a translation gets the accessed flag (if not already set) in the
 * directory CR3 names; the dirty flag only in the leaf PTE. The isolation
 * probe's check (src/kernel/probes/probes.c probe_isolation) is applied to
 * two directories cloned around runtime kernel allocations, replaying the
 * QEMU run of image 8af310c2 (cr3_b=01bff000, pfn_b=7168: the first page of
 * the 28-32 MiB direct-map region was zeroed after both clones).
 * No privileged instruction is executed. ASan/UBSan host harness.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL kernel_pde:%d %s\n", __LINE__, #x); failures++; } } while (0)
struct ciuki_boot_info g_boot;
static uint32_t host_cr3;
static uint32_t host_read_cr3(void) { return host_cr3; }
static void host_write_cr3(uint32_t value) { host_cr3 = value; }
static void host_invlpg(uint32_t va) { (void)va; }
static uint32_t host_irq_save(void) { return 0x200; }
static void host_irq_restore(uint32_t flags) { (void)flags; }
void klog(const char *fmt, ...) { (void)fmt; }
void panic(const char *fmt, ...) { printf("panic: %s\n", fmt); abort(); }
void kpreempt_point(void) { }

/* Synthetic physical memory for page tables and directories only; the
 * direct map's RAM itself is never dereferenced by the model. */
static uint8_t ram[4 * 1024 * 1024] __attribute__((aligned(4096)));
static uint32_t next_page = 0x1000;
uint32_t pmm_alloc(void)
{
    if (next_page >= sizeof(ram)) return 0;
    uint32_t p = next_page; next_page += PAGE_SIZE; return p;
}
uint32_t pmm_alloc_zone(int zone) { (void)zone; return pmm_alloc(); }
void pmm_free(uint32_t phys) { (void)phys; }

#undef irq_save
#undef irq_restore
#define irq_save host_irq_save
#define irq_restore host_irq_restore
#define read_cr3 host_read_cr3
#define write_cr3 host_write_cr3
#define invlpg host_invlpg
#undef P2V
#define P2V(p) ((void *)(ram + (uint32_t)(p)))
#define __rodata_end __text_start        /* no kernel image to protect here */
char __text_start[1];
#include "../../src/kernel/core/vmm.c"

/* One translation by the CPU under `cr3`: SDM 4.8 accessed/dirty rules. */
static void cpu_access(uint32_t cr3, uint32_t va, bool write)
{
    uint32_t *pd = P2V(cr3), *pde = &pd[va >> 22];
    CHECK(*pde & PTE_P);
    if (!(*pde & PTE_P)) return;
    *pde |= PTE_A;
    uint32_t *pte = &((uint32_t *)P2V(*pde & ~0xFFFu))[(va >> 12) & 1023];
    CHECK(*pte & PTE_P);
    *pte |= PTE_A | (write ? PTE_D : 0);
}
/* The kernel zeroes `bytes` of a fresh allocation through the direct map. */
static void kernel_zero(uint32_t cr3, uint32_t phys, uint32_t bytes)
{
    for (uint32_t at = 0; at < bytes; at += PAGE_SIZE) cpu_access(cr3, KERNEL_VBASE + phys + at, true);
}
/* probe_isolation: for i in 768..1022, pda[i] != kpd[i] || pdb[i] != kpd[i] || (pda[i] & PTE_U). */
static unsigned shared_kernel_errors(const struct aspace *a, const struct aspace *b, unsigned *first)
{
    uint32_t *pda = P2V(a->pd_phys), *pdb = P2V(b->pd_phys), *k = P2V(vmm_kernel_pd());
    unsigned errors = 0;
    *first = 0;
    for (unsigned i = 768; i < 1023; i++)
        if (pda[i] != k[i] || pdb[i] != k[i] || (pda[i] & PTE_U)) {
            if (!errors) *first = i;
            errors++;
        }
    return errors;
}
static void boot(bool preset)
{
    memset(ram, 0, sizeof(ram)); next_page = 0x1000;
    memset(&g_boot, 0, sizeof(g_boot)); mmio_next = MMIO_REGION; memset(stack_slot_used, 0, sizeof(stack_slot_used));
    sample_spaces = sample_space = 0; sample_va = 0;    /* the production sampler registry */
    g_boot.e820_count = 1;
    g_boot.e820[0] = (struct ciuki_e820){.base = 0, .length = 512u << 20, .type = CBI_E820_RAM};
    vmm_init();
    CHECK(host_cr3 == vmm_kernel_pd());
    if (!preset)                         /* the directory before f6-21b: no accessed flag */
        for (unsigned i = 768; i < 1023; i++) kpd[i] &= ~PTE_A;
    /* Boot and drivers used 16-28 MiB before the probe (the QEMU ledger:
     * about 3048 ZONE_NORMAL pages); 28 MiB and above is still untouched. */
    for (uint32_t p = 0x1000000u; p < 0x1C00000u; p += 0x400000u) kernel_zero(vmm_kernel_pd(), p, PAGE_SIZE);
}
/* The QEMU run: spawn(iso-a) clones, then zeroes its pages at pfn 7154;
 * spawn(iso-b) clones (pd at pfn 7167), then zeroes its data page at pfn
 * 7168 = 0x1C00000, the first page of PDE 775. Then the jitter task zeroes
 * its 256 KiB buffer in a fresh region, and process a's thread touches yet
 * another fresh kernel region in a syscall under its own CR3. */
static unsigned replay(bool preset, unsigned *after_spawn, unsigned *first_index, uint32_t *difference)
{
    boot(preset);
    static struct aspace a, b;
    CHECK(!as_create(&a));
    kernel_zero(vmm_kernel_pd(), 7154u << 12, 3 * PAGE_SIZE);
    CHECK(!as_create(&b));
    kernel_zero(vmm_kernel_pd(), 7168u << 12, 3 * PAGE_SIZE);
    *after_spawn = shared_kernel_errors(&a, &b, first_index);
    *difference = ((uint32_t *)P2V(a.pd_phys))[*first_index] ^ kpd[*first_index];
    kernel_zero(vmm_kernel_pd(), 0x2000000u, 262144);        /* jitter buffer, 32 MiB */
    kernel_zero(a.pd_phys, 0x2400000u, PAGE_SIZE);            /* a syscall in process a, 36 MiB */
    unsigned first;
    unsigned errors = shared_kernel_errors(&a, &b, &first);
    /* Kernel PTEs are shared tables: the accessed/dirty bits of leaves are
     * one copy, whichever directory walked them. */
    CHECK(vmm_kernel_pte(KERNEL_VBASE + 0x2400000u) & PTE_A);
    return errors;
}
int main(void)
{
    unsigned spawn_errors, index, total; uint32_t xor;
    total = replay(false, &spawn_errors, &index, &xor);
    printf("f6-21b negative control (PDE without the accessed flag): after the two spawns shared_kernel_errors=%u first=%u xor=%03x; "
           "after the jitter buffer and a syscall touch %u\n", spawn_errors, index, xor, total);
    CHECK(spawn_errors == 1 && index == 775 && xor == PTE_A);   /* the QEMU record, exactly */
    CHECK(total == 3);
    total = replay(true, &spawn_errors, &index, &xor);
    CHECK(spawn_errors == 0 && total == 0);
    for (unsigned i = 768; i < 1023; i++) CHECK((kpd[i] & (PTE_P | PTE_W | PTE_A | PTE_U)) == (PTE_P | PTE_W | PTE_A));
    CHECK((kpd[1023] & ~0xFFFu) == vmm_kernel_pd() && !(kpd[1023] & PTE_U));
    if (failures) { printf("kernel_pde: %u/%u checks failed\n", failures, checks); return 1; }
    printf("f6-21b kernel PDEs: preset accessed flag keeps 768-1022 identical in every directory across runtime kernel "
           "allocations (probe spawn, 256 KiB jitter buffer, syscall under a process CR3); %u checks PASS\n", checks);
    return 0;
}
