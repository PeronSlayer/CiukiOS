/* Resource registry: legacy reservations before PCI, read-only PCI
 * inventory through configuration mechanism #1 (no BAR sizing, no
 * activation), transactional claims that fail without side effects on
 * overlap (docs/design/device-firmware-ownership.md).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/registry.h>

#define MAX_RES 64
#define MAX_PCI 32

static struct resource res[MAX_RES];
static unsigned nres;
static uint32_t gen_counter = 1;
static struct pci_func pci[MAX_PCI];
static unsigned npci;

static bool overlaps(const struct resource *r, enum res_type t, uint32_t s, uint32_t e)
{
    return r->type == t && r->state != RS_RELEASED && s < r->end && r->start < e;
}

int registry_claim(enum res_type type, uint32_t start, uint32_t end, const char *owner, bool share)
{
    if (end <= start)
        return -EINVAL;
    uint32_t f = irq_save();
    for (unsigned i = 0; i < nres; i++) {
        if (overlaps(&res[i], type, start, end) && !(share && res[i].shareable)) {
            irq_restore(f);
            klog("[registry] conflict: %s wants %u:%x-%x held by %s", owner, type, start, end, res[i].owner);
            return -EINVAL;
        }
    }
    if (nres == MAX_RES) {
        irq_restore(f);
        return -ENOMEM;
    }
    res[nres] = (struct resource){ type, start, end, owner, gen_counter++, RS_CLAIMED, share };
    int h = (int)nres++;
    irq_restore(f);
    return h;
}

int registry_release(int h, uint32_t generation)
{
    if (h < 0 || (unsigned)h >= nres || res[h].generation != generation || res[h].state == RS_RELEASED)
        return -EINVAL;
    res[h].state = RS_RELEASED;
    return 0;
}

unsigned registry_count(void) { return nres; }
const struct resource *registry_get(unsigned i) { return i < nres ? &res[i] : 0; }
unsigned pci_count(void) { return npci; }
const struct pci_func *pci_get(unsigned i) { return i < npci ? &pci[i] : 0; }

static uint32_t pci_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    uint32_t f = irq_save();
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                ((uint32_t)fn << 8) | (off & 0xFC));
    uint32_t v = inl(0xCFC);
    irq_restore(f);
    return v;
}

static void pci_scan(void)
{
    /* Bus 0 and firmware-configured secondary buses of PCI-PCI bridges. */
    uint8_t buses[8] = { 0 };
    unsigned nb = 1;
    for (unsigned bi = 0; bi < nb; bi++) {
        uint8_t bus = buses[bi];
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint32_t id0 = pci_read(bus, dev, 0, 0);
            if ((id0 & 0xFFFF) == 0xFFFF)
                continue;
            uint8_t ht0 = (pci_read(bus, dev, 0, 0x0C) >> 16) & 0xFF;
            uint8_t nfn = (ht0 & 0x80) ? 8 : 1;
            for (uint8_t fn = 0; fn < nfn; fn++) {
                uint32_t id = pci_read(bus, dev, fn, 0);
                if ((id & 0xFFFF) == 0xFFFF || npci == MAX_PCI)
                    continue;
                struct pci_func *p = &pci[npci++];
                memset(p, 0, sizeof(*p));
                p->bus = bus; p->dev = dev; p->fn = fn;
                p->vendor = id & 0xFFFF;
                p->device = id >> 16;
                uint32_t cr = pci_read(bus, dev, fn, 0x08);
                p->revision = cr & 0xFF;
                p->prog_if = (cr >> 8) & 0xFF;
                p->subclass = (cr >> 16) & 0xFF;
                p->class_code = cr >> 24;
                p->header_type = (pci_read(bus, dev, fn, 0x0C) >> 16) & 0xFF;
                p->command = pci_read(bus, dev, fn, 0x04) & 0xFFFF;
                if ((p->header_type & 0x7F) == 0) {
                    for (int b = 0; b < 6; b++)
                        p->bar[b] = pci_read(bus, dev, fn, 0x10 + b * 4);
                    uint32_t ss = pci_read(bus, dev, fn, 0x2C);
                    p->sub_vendor = ss & 0xFFFF;
                    p->sub_device = ss >> 16;
                    uint32_t il = pci_read(bus, dev, fn, 0x3C);
                    p->irq_line = il & 0xFF;
                    p->irq_pin = (il >> 8) & 0xFF;
                } else if ((p->header_type & 0x7F) == 1) {
                    uint8_t sec = (pci_read(bus, dev, fn, 0x18) >> 8) & 0xFF;
                    bool seen = false;
                    for (unsigned k = 0; k < nb; k++)
                        if (buses[k] == sec)
                            seen = true;
                    if (sec && !seen && nb < ARRAY_SIZE(buses))
                        buses[nb++] = sec;
                }
            }
        }
    }
}

/* 8237 pair: mask every channel except the cascade (4) so no inherited
 * firmware programming can transfer; port 0Ah masks 0-3, D4h masks 4-7. */
static void dma_init(void)
{
    static const uint8_t ch[] = { 0, 1, 2, 3, 5, 6, 7 };
    for (unsigned i = 0; i < ARRAY_SIZE(ch); i++) {
        if (ch[i] < 4)
            outb(0x0A, 0x04 | ch[i]);
        else
            outb(0xD4, 0x04 | (ch[i] - 4));
    }
}

void registry_init(void)
{
    dma_init();
    nres = 0;
    static const struct { uint32_t s, e; const char *o; } legacy[] = {
        { 0x20, 0x22, "pic" }, { 0xA0, 0xA2, "pic" },
        { 0x40, 0x44, "timer" }, { 0x61, 0x62, "timer/speaker" },
        { 0x70, 0x72, "rtc" }, { 0x92, 0x93, "platform-a20" },
        { 0x60, 0x61, "input" }, { 0x64, 0x65, "input" },
        { 0x00, 0x20, "dma" }, { 0x80, 0x90, "dma-page" }, { 0xC0, 0xE0, "dma" },
        { 0xCF8, 0xD00, "pci" },
    };
    for (unsigned i = 0; i < ARRAY_SIZE(legacy); i++) {
        int h = registry_claim(RES_PORT, legacy[i].s, legacy[i].e, legacy[i].o, false);
        if (h >= 0)
            res[h].state = RS_FIRMWARE;
    }
    registry_claim(RES_IRQ, 0, 1, "timer", false);
    registry_claim(RES_IRQ, 2, 3, "pic-cascade", false);
    registry_claim(RES_DMA, 4, 5, "dma-cascade", false);
    if (g_boot.uart_base)
        registry_claim(RES_PORT, g_boot.uart_base, g_boot.uart_base + 8, "uart", false);
    if (g_boot.fb_phys && !(g_boot.flags & CBI_F_TEXT_MODE))
        registry_claim(RES_MMIO, g_boot.fb_phys, g_boot.fb_phys + g_boot.fb_pitch * g_boot.fb_height,
                       "boot-framebuffer", false);
    /* PCI BIOS reported configuration mechanism #1 (AL bit 0). */
    if ((g_boot.pci_bios & 1) && ((g_boot.pci_bios >> 8) & 1))
        pci_scan();
    klog("[registry] %u reservations, %u PCI functions (read-only)", nres, npci);
}
