/* Ciuki UP backend. Inventory writes are denied; power uses exact live leases.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "runtime.h"
#include <ciuki/kernel.h>
#include <ciuki/mm.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/timing.h>
#include <ciuki/sync.h>
#include <ciuki/com.h>
#include <ciuki/registry.h>
#include <ciuki/thinkpad_ec.h>
#include <ciuki/power_source.h>
extern void audio_ec_attach(void);

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
static ACPI_STATUS write_access(unsigned space, uint64_t a, unsigned width, UINT64 value, const ACPI_PCI_ID *pci)
{
    /* No generic MMIO/PCI write grants: firmware AML cannot seize another
     * native driver's registers. PM and EC access is exact SystemIO only. */
    if (space || !fw_region_check(space, a, width, pci)) return AE_ACCESS;
    uint32_t f = irq_save();
    if (!fw_region_check(space, a, width, pci)) { irq_restore(f); return AE_ACCESS; }
    if (width == 8) outb(a, value); else if (width == 16) cpu_outw(a, value);
    else if (width == 32) outl(a, value); else { irq_restore(f); return AE_BAD_PARAMETER; }
    irq_restore(f); return AE_OK;
}
static struct owned_claim { int handle; uint32_t generation; } claims[16];
static unsigned claim_count;
static int sci_claim = -1;
static unsigned sci_number;
static ACPI_OSD_HANDLER sci_handler;
static void *sci_context;
static bool sci_attached;
static ACPI_STATUS claim_port(uint32_t a, uint32_t n)
{
    if (!n || a > 65535 || n > 65536 - a || claim_count == 16) return AE_BAD_ADDRESS;
    for (unsigned i = 0; i < claim_count; i++) {
        const struct resource *r = registry_get(claims[i].handle);
        if (registry_valid(claims[i].handle, claims[i].generation) && r->type == RES_PORT && r->start <= a && r->end >= a + n) return AE_OK;
    }
    int h = registry_claim(RES_PORT, a, a + n, "acpi-power", false);
    if (h < 0) return AE_ACCESS;
    uint32_t gen = registry_get(h)->generation;
    claims[claim_count++] = (struct owned_claim){h, gen};
    if (registry_activate(h, gen) || fw_region_grant(0, h, gen, NULL)) return AE_ACCESS;
    return AE_OK;
}
static bool sci_irq(unsigned irq, const char *owner)
{
    (void)owner;
    if (!sci_handler || irq != sci_number || !fw_writes) return false;
    fw_interrupt = true;
    UINT32 handled = sci_handler(sci_context);
    fw_interrupt = false;
    return handled == ACPI_INTERRUPT_HANDLED;
}
static ACPI_STATUS sci(UINT32 n, ACPI_OSD_HANDLER handler, void *context)
{
    /* ACPI's SCI_INT is a PIC input on this supported legacy model. Require
     * firmware-programmed level polarity in ELCR; never change routing. */
    if (n < 3 || n >= 16 || n == 8 || n == 13 || !g_cpu_tsc || !g_tsc_per_ms ||
        !(inb(0x4d0 + n / 8) & (1u << (n & 7)))) return AE_SUPPORT;
    if (sci_claim < 0) {
        if (claim_count == 16) return AE_LIMIT;
        int h = registry_claim(RES_IRQ, n, n + 1, "acpi-power", true);
        if (h < 0) return AE_ACCESS;
        uint32_t gen = registry_get(h)->generation;
        claims[claim_count++] = (struct owned_claim){h, gen}; sci_claim = h; sci_number = n;
        if (registry_activate(h, gen)) return AE_ACCESS;
    }
    if (n != sci_number) return AE_SUPPORT;
    const struct resource *route = registry_get(sci_claim);
    if (!route || route->state != RS_ACTIVE) return AE_ACCESS;
    if (!handler) return AE_OK; /* qualification before the ACPI_ENABLE write */
    if (sci_attached) return AE_ALREADY_EXISTS;
    sci_handler = handler; sci_context = context;
    if (irq_chain_add(n, sci_irq, "acpi-power")) { sci_handler = NULL; return AE_ACCESS; }
    sci_attached = true; return AE_OK;
}
static ACPI_STATUS unsci(UINT32 n, ACPI_OSD_HANDLER handler)
{
    if (!sci_attached) return AE_OK;
    if (n != sci_number || (handler && handler != sci_handler) || irq_chain_remove(n, sci_irq, "acpi-power")) return AE_ERROR;
    sci_attached = false; sci_handler = NULL; sci_context = NULL; return AE_OK;
}
static bool idle(int handle, uint32_t generation)
{ (void)handle; (void)generation; return !sci_attached; }
static void finish(bool safe)
{
    for (unsigned i = 0; i < claim_count; i++) {
        struct owned_claim *c = &claims[i];
        if (!safe || registry_quiesce(c->handle, c->generation, idle) || registry_release(c->handle, c->generation))
            (void)registry_quarantine(c->handle, c->generation);
    }
    fw_regions_reset(); claim_count = 0; sci_claim = -1;
}
/* ACPI as one provider of ciuki/power_source.h. Soft-off reaches the S5
 * worker only through power_source_soft_off, after the call-73 latch. */
static int acpi_soft_off(void *ctx) { (void)ctx; return acpi_power_off_owned(); }
static bool acpi_can_off(void *ctx) { (void)ctx; return acpi_power_can_off(); }
static int provider_reserve(void)
{
    const struct power_source_ops ops = {.soft_off = acpi_soft_off, .can_off = acpi_can_off};
    return power_source_reserve(POWER_SOURCE_ACPI, &ops);
}
static int provider_activate(void) { return power_source_activate(POWER_SOURCE_ACPI); }
static void provider_retire(bool uncertain) { power_source_release(POWER_SOURCE_ACPI, uncertain, uncertain ? -EIO : 0); }
static void provider_publish(const struct ciuki_power_status *p) { power_source_publish_record(POWER_SOURCE_ACPI, p); }
const struct fw_ops fw_kernel_ops = {allocate, release, charge, map, unmap, clock100ns, pause_ms, udelay, thread, lock, unlock, access,
    write_access, claim_port, sci, unsci, finish, provider_reserve, provider_activate, provider_retire, provider_publish};
static void com_pnp_thread(void *unused)
{
    (void)unused;
    com_peripherals_start();
}
static void inventory_thread(void *unused)
{
    (void)unused;
    ACPI_STATUS s = (g_boot.flags & (CBI_F_SAFE_MODE | CBI_F_HW_CHECK)) ?
        fw_inventory_run(&fw_kernel_ops, g_boot.acpi_rsdp_phys) :
        fw_inventory_power_run(&fw_kernel_ops, g_boot.acpi_rsdp_phys);
    struct ciuki_firmware_page p = {.version = CIUKI_FIRMWARE_VERSION, .size = sizeof(p)};
    (void)firmware_inventory_page(&p);
    klog("[acpi] inventory status=%u devices=%u tables=%u peak=%u flags=%u power_owner=%u", s, p.total, p.table_count, p.peak_bytes, p.flags, fw_power.ownership);
    /* Discovery stays read-only for devices. Import assigned routes from the
     * completed inventory records (the interpreter is terminated, or retained
     * only by the power owner); the COM owner claims before probing. */
    if (!(p.flags&(CIUKI_FIRMWARE_PENDING|CIUKI_FIRMWARE_INCOMPLETE|CIUKI_FIRMWARE_TRUNCATED))) {
        do {
            for (unsigned i=0;i<p.count;i++) (void)com_firmware_assigned(&p.records[i]);
            p.cursor=p.next;
            if (!p.cursor) break;
        } while (!firmware_inventory_page(&p));
    }
    /* fx-3: the EC quirk attaches once ownership is decided, owned or not
     * (on a firmware-owned platform through the legacy EC binding), before
     * serial PnP can spend seconds here. */
    audio_ec_attach();
    /* Serial PnP enumeration waits seconds per port; once ACPI owns SCI,
     * thermal and button events, it must not delay this thread's poll. */
    struct task *com = fw_owned ? task_create_kernel("com-pnp", com_pnp_thread, NULL, P_DEVICE) : NULL;
    if (com) task_start(com);
    else com_peripherals_start();
    while (fw_owned) { fw_power_poll(); task_sleep_ms(20); }
}
void firmware_inventory_start(void)
{
    static bool attempted;
    if (attempted) return;
    attempted = true;
    struct task *t = task_create_kernel("acpi-power", inventory_thread, NULL, P_DEVICE);
    /* Bound so the unavailable record also reaches the power_source cache. */
    if (!t) { fw_bind(&fw_kernel_ops); fw_inventory_unavailable(AE_NO_MEMORY); audio_ec_attach(); }
    else task_start(t);
}
