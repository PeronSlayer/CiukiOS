/* Ciuki UP backend. All inventory hardware writes are denied by the OSL.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "internal.h"
#include <ciuki/kernel.h>
#include <ciuki/mm.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/timing.h>
#include <ciuki/sync.h>
#include <ciuki/com.h>

struct large { uint32_t pages, physical; uint64_t reserved; };
static size_t charge(size_t n)
{
    n += sizeof(struct large);
    if (n <= 2000) { size_t c = 32; while (c < n + 8) c *= 2; return c; }
    return PAGE_ALIGN_UP(n);
}
static void *allocate(size_t n)
{
    size_t total = n + sizeof(struct large); struct large *h;
    if (total <= 2000) {
        h = kmalloc(total); if (!h) return NULL; *h = (struct large){0};
    } else {
        uint32_t pages = PAGE_ALIGN_UP(total) / PAGE_SIZE, phys = pmm_alloc_contig(pages);
        if (!phys) return NULL;
        mm_claim(phys, pages, MM_HEAP); h = P2V(phys); *h = (struct large){pages, phys, 0};
    }
    return h + 1;
}
static void release(void *p)
{
    struct large *h = (struct large *)p - 1;
    if (!h->pages) kfree(h);
    else { uint32_t n = h->pages, phys = h->physical; for (uint32_t i = 0; i < n; i++) mm_free(phys + i * PAGE_SIZE, MM_HEAP); }
}
static const void *map(uint32_t a, uint32_t n)
{
    if (!n || a > UINT32_MAX - n) return NULL;
    /* Reject kernel-owned RAM and reserved device BARs. Firmware tables
     * must be in the BIOS/EBDA window or E820 ACPI reclaim/NVS regions. */
    bool firmware = (a >= 0xe0000 && a + n <= 0x100000) ||
                    (a >= 0x80000 && a + n <= 0xa0000);
    for (unsigned i = 0; i < g_boot.e820_count; i++) {
        const struct ciuki_e820 *e = &g_boot.e820[i];
        if ((e->type == 3 || e->type == 4) && e->base <= a &&
            e->length <= UINT64_MAX - e->base && e->base + e->length >= (uint64_t)a + n) firmware = true;
    }
    if (!firmware) return NULL;
    for (uint64_t p = PAGE_ALIGN_DOWN(a); p < (uint64_t)a + n; p += PAGE_SIZE)
        if (!pmm_is_reserved(p)) return NULL;
    return vmm_map_physical_ro(a, n);
}
static void unmap(const void *p, uint32_t n) { vmm_revoke_mmio((void *)p, n); }
static uint64_t clock100ns(void)
{
    static uint64_t last;
    uint32_t f = irq_save();
    uint64_t now = g_ticks * 10000;
    if (g_cpu_tsc && g_tsc_per_ms) {
        uint64_t cycles = read_tsc_unserialized();
        now = (cycles / g_tsc_per_ms) * 10000 + ((cycles % g_tsc_per_ms) * 10000) / g_tsc_per_ms;
    }
    if (now < last) now = last; else last = now;
    irq_restore(f); return now;
}
static void pause_ms(uint32_t n) { if (n) task_sleep_ms(n); else task_yield(); }
static uint64_t thread(void) { return g_current ? g_current->id : 1; }
static uint32_t lock(void) { return irq_save(); }
static void unlock(uint32_t f) { irq_restore(f); }
static ACPI_STATUS access(unsigned space, uint64_t a, unsigned width, UINT64 *value, const ACPI_PCI_ID *pci)
{
    if (!fw_region_check(space, a, width, pci)) return AE_ACCESS;
    if (space == 0) {
        uint32_t f = irq_save();
        if (!fw_region_check(space, a, width, pci)) { irq_restore(f); return AE_ACCESS; }
        *value = width == 8 ? inb(a) : width == 16 ? cpu_inw(a) : inl(a);
        irq_restore(f); return AE_OK;
    }
    if (space == 1) {
        const volatile uint8_t *p = vmm_map_mmio(a, width / 8, true);
        if (!p) return AE_NO_MEMORY;
        uint32_t f = irq_save();
        if (!fw_region_check(space, a, width, pci)) { irq_restore(f); vmm_revoke_mmio((void *)p, width / 8); return AE_ACCESS; }
        *value = width == 8 ? *p : width == 16 ? *(const volatile uint16_t *)p :
                 width == 32 ? *(const volatile uint32_t *)p : *(const volatile uint64_t *)p;
        irq_restore(f);
        vmm_revoke_mmio((void *)p, width / 8); return AE_OK;
    }
    uint32_t f = irq_save();
    /* CF8 selection is the sole PCI mechanism write; preserve the selector. */
    if (!fw_region_check(space, a, width, pci)) { irq_restore(f); return AE_ACCESS; }
    uint32_t old = inl(0xcf8);
    outl(0xcf8, 0x80000000u | pci->Bus << 16 | pci->Device << 11 | pci->Function << 8 | ((uint32_t)a & ~3u));
    uint32_t data = inl(0xcfc); outl(0xcf8, old); irq_restore(f);
    *value = (data >> ((a & 3) * 8)) & (width == 8 ? 255u : width == 16 ? 65535u : UINT32_MAX);
    return AE_OK;
}
const struct fw_ops fw_kernel_ops = {allocate, release, charge, map, unmap, clock100ns, pause_ms, udelay, thread, lock, unlock, access};
static void inventory_thread(void *unused)
{
    (void)unused;
    ACPI_STATUS s = fw_inventory_run(&fw_kernel_ops, g_boot.acpi_rsdp_phys);
    struct ciuki_firmware_page p = {.version = CIUKI_FIRMWARE_VERSION, .size = sizeof(p)};
    (void)firmware_inventory_page(&p);
    klog("[acpi] inventory status=%u devices=%u tables=%u peak=%u flags=%u read_only=1", s, p.total, p.table_count, p.peak_bytes, p.flags);
    /* Discovery itself stays read-only. Import assigned routes only after
     * the interpreter is terminated; the COM owner claims before probing. */
    if (!(p.flags&(CIUKI_FIRMWARE_PENDING|CIUKI_FIRMWARE_INCOMPLETE|CIUKI_FIRMWARE_TRUNCATED))) {
        do {
            for (unsigned i=0;i<p.count;i++) (void)com_firmware_assigned(&p.records[i]);
            p.cursor=p.next;
            if (!p.cursor) break;
        } while (!firmware_inventory_page(&p));
    }
    com_peripherals_start();
}
void firmware_inventory_start(void)
{
    static bool attempted;
    if (attempted) return;
    attempted = true;
    if (!task_create_kernel("acpi-inventory", inventory_thread, NULL, P_DEVICE))
        fw_inventory_unavailable(AE_NO_MEMORY);
}
