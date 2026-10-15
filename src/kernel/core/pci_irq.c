/* Read-only PCI INTx route qualification. Sources: f1-71 validation record.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/registry.h>

static uint32_t intx_config(const struct pci_func *p, unsigned off)
{
    uint32_t f = irq_save();
    outl(0xcf8, 0x80000000u | (uint32_t)p->bus << 16 | (uint32_t)p->dev << 11 |
         (uint32_t)p->fn << 8 | (off & 0xfc));
    uint32_t v = inl(0xcfc);
    irq_restore(f);
    return v;
}
static bool intx_route(const struct pci_func *router, unsigned reg, unsigned irq)
{
    unsigned route = (intx_config(router, reg) >> ((reg & 3) * 8)) & 255;
    /* QEMU PIIX treats the whole byte as the IRQ and rejects values >=16.
     * ICH defines bit 7 as disable and ignores reserved bits 6:4. */
    if (router->device == 0x7000 || router->device == 0x7110) return route == irq;
    return !(route & 0x80) && (route & 15) == irq;
}
static unsigned intx_le16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
static bool intx_pir(const struct pci_func *p, const uint8_t *table, unsigned available)
{
    if (available < 48 || memcmp(table, "$PIR", 4) || intx_le16(table + 4) != 0x100) return false;
    unsigned size = intx_le16(table + 6), sum = 0;
    if (size < 48 || size > available || size > 4096 || size % 16) return false;
    for (unsigned i = 0; i < size; i++) sum += table[i];
    if (sum & 255) return false;
    const struct pci_func *router = 0;
    for (unsigned i = 0; i < pci_count(); i++) {
        const struct pci_func *r = pci_get(i);
        if (r->bus == table[8] && r->dev * 8u + r->fn == table[9] &&
            r->vendor == 0x8086 && r->class_code == 6 && r->subclass == 1 &&
            (r->device == 0x7000 || r->device == 0x7110 || r->device == 0x2410 ||
             r->device == 0x2420 || r->device == 0x2440 || r->device == 0x244c ||
             r->device == 0x2480 || r->device == 0x248c || r->device == 0x24c0 || r->device == 0x24cc))
            router = r;
    }
    if (!router) return false;
    for (unsigned i = 32; i < size; i += 16) {
        if (table[i] != p->bus || table[i + 1] >> 3 != p->dev) continue;
        unsigned at = i + 2 + (p->irq_pin - 1) * 3, link = table[at];
        bool ich = router->device != 0x7000 && router->device != 0x7110;
        if (!((link >= 0x60 && link <= 0x63) || (ich && link >= 0x68 && link <= 0x6b)) ||
            !(intx_le16(table + at + 1) & (1u << p->irq_line))) return false;
        return intx_route(router, link, p->irq_line);
    }
    return false;
}
bool pci_irq_routed(const struct pci_func *p)
{
    if (!p || !p->irq_pin || p->irq_pin > 4 || p->irq_line >= 16 ||
        !(0xdef8u & (1u << p->irq_line)) || (p->command & 0x400)) return false;
    if (!(inb(0x4d0 + p->irq_line / 8) & (1u << (p->irq_line % 8)))) return false;
    for (unsigned i = 0; i < pci_count(); i++) {
        const struct pci_func *r = pci_get(i);
        if (r->bus || r->vendor != 0x8086 || r->class_code != 6 || r->subclass != 1) continue;
        if (!p->bus && (g_boot.flags & CBI_F_SMBIOS_QEMU) &&
            (r->device == 0x7000 || r->device == 0x7110))
            return intx_route(r, 0x60 + ((p->dev + p->irq_pin - 2) & 3), p->irq_line);
        /* ICH3-M's integrated functions have fixed internal routes. */
        if (r->dev == 31 && !r->fn && r->device == 0x248c && p->vendor == 0x8086) {
            static const uint16_t usb_ids[3] = {0x2482, 0x2484, 0x2487};
            static const uint8_t usb_routes[3] = {0x60, 0x63, 0x62};
            if (!p->bus && p->dev == 29 && p->fn < 3 && p->device == usb_ids[p->fn] &&
                p->class_code == 12 && p->subclass == 3 && !p->prog_if && p->irq_pin == p->fn + 1)
                return intx_route(r, usb_routes[p->fn], p->irq_line);
            if (!p->bus && p->dev == 31 && p->fn == 5 && p->device == 0x2485 && p->irq_pin == 2)
                return intx_route(r, 0x61, p->irq_line);
            /* Integrated LAN lives behind D30, on its firmware-assigned
             * secondary bus (ICH3 §6.1), not on bus zero. */
            if (p->bus && p->dev == 8 && !p->fn && p->class_code == 2 && !p->subclass && p->irq_pin == 1 &&
                (p->device == 0x2449 || p->device == 0x1031 || p->device == 0x1032 ||
                 p->device == 0x1033 || p->device == 0x1035)) {
                for (unsigned j = 0; j < pci_count(); j++) {
                    const struct pci_func *b = pci_get(j);
                    if (b->bus || b->dev != 30 || b->fn || b->vendor != 0x8086 || b->device != 0x2448 ||
                        b->class_code != 6 || b->subclass != 4) continue;
                    unsigned buses = intx_config(b, 0x18);
                    if (!(buses & 255) && ((buses >> 8) & 255) == p->bus && ((buses >> 16) & 255) >= p->bus)
                        return intx_route(r, 0x68, p->irq_line);
                }
            }
        }
    }
    for (unsigned off = 0; off <= 0x10000 - 48; off += 16)
        if (intx_pir(p, (const uint8_t *)P2V(0xf0000) + off, 0x10000 - off)) return true;
    return false;
}
