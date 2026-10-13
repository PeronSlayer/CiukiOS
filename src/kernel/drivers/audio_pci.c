/* Firmware PCI INTx routes; no route writes. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/audio_backend.h>
uint32_t audio_pci_read(const struct ac97_io *io, const struct pci_func *p, unsigned off)
{
    uint32_t flags = irq_save();
    io->write(io->cookie, 0xcf8, 0x80000000u | (uint32_t)p->bus << 16 | (uint32_t)p->dev << 11 |
              (uint32_t)p->fn << 8 | (off & 0xfc), 4);
    uint32_t value = io->read(io->cookie, 0xcfc, 4);
    irq_restore(flags); return value;
}
void audio_pci_write(const struct ac97_io *io, const struct pci_func *p, unsigned off, uint32_t value, unsigned width)
{
    uint32_t flags = irq_save();
    io->write(io->cookie, 0xcf8, 0x80000000u | (uint32_t)p->bus << 16 | (uint32_t)p->dev << 11 |
              (uint32_t)p->fn << 8 | (off & 0xfc), 4);
    io->write(io->cookie, 0xcfc + (off & 3), value, width);
    irq_restore(flags);
}
static uint16_t audio_le16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)p[1] << 8; }
static bool audio_pir_route(const struct ac97_io *io, const struct pci_func *p, const uint8_t *table, unsigned available)
{
    if (available < 32 || memcmp(table, "$PIR", 4) || audio_le16(table + 4) != 0x100) return false;
    unsigned size = audio_le16(table + 6);
    if (size < 48 || size > available || size > 4096 || size % 16) return false;
    unsigned sum = 0; for (unsigned i = 0; i < size; i++) sum += table[i];
    if (sum & 255) return false;
    const struct pci_func *router = 0;
    for (unsigned i = 0; i < pci_count(); i++) {
        const struct pci_func *r = pci_get(i);
        if (r->bus == table[8] && (r->dev * 8u + r->fn) == table[9] && r->class_code == 6 && r->subclass == 1) router = r;
    }
    if (!router) return false;
    for (unsigned i = 32; i < size; i += 16) {
        if (table[i] != p->bus || (table[i + 1] >> 3) != p->dev) continue;
        unsigned at = i + 2 + (p->irq_pin - 1) * 3;
        unsigned link = table[at];
        if (!link || !(audio_le16(table + at + 1) & (1u << p->irq_line))) return false;
        unsigned route = 0;
        if (router->vendor == 0x8086 && link >= 0x60 && link <= 0x63) {
            route = (audio_pci_read(io, router, link) >> ((link & 3) * 8)) & 255;
            return route == p->irq_line;
        }
        if (router->vendor == 0x1106 && link <= 4 &&
            (router->device == 0x0686 || router->device == 0x3074 || router->device == 0x3147 || router->device == 0x3177)) {
            unsigned nibble = link == 4 ? 5 : link;
            unsigned reg = 0x55 + nibble / 2;
            route = (audio_pci_read(io, router, reg) >> ((reg & 3) * 8 + (nibble & 1) * 4)) & 15;
            return route == p->irq_line;
        }
        if (router->vendor == 0x1039 && router->device == 0x0008) {
            /* Normal INTA-D links only; on-chip SiS links have revision-specific
             * layouts and are not guessed. */
            unsigned reg = link <= 4 ? link + 0x40 : link;
            if (reg < 0x41 || reg > 0x44) return false;
            route = (audio_pci_read(io, router, reg) >> ((reg & 3) * 8)) & 255;
            return !(route & 0x80) && (route & 15) == p->irq_line;
        }
        /* nForce firmware tables describe chipset-private links. Require the
         * BIOS-assigned line, matching pin bitmap and ELCR; never steer it. */
        return router->vendor == 0x10de && p->vendor == 0x10de &&
               (p->device == 0x01b1 || p->device == 0x006a);
    }
    return false;
}
bool audio_pci_routed(const struct ac97_io *io, const struct pci_func *p)
{
    if (!p->irq_pin || p->irq_pin > 4 || p->irq_line >= 16 || !(0xdef8u & (1u << p->irq_line))) return false;
    if (!(io->read(io->cookie, 0x4d0 + p->irq_line / 8, 1) & (1u << (p->irq_line % 8)))) return false;
    if (!p->bus && (g_boot.flags & CBI_F_SMBIOS_QEMU)) {
        for (unsigned i = 0; i < pci_count(); i++) {
            const struct pci_func *r = pci_get(i);
            if (r->vendor != 0x8086 || r->bus || r->class_code != 6 || r->subclass != 1 ||
                (r->device != 0x7000 && r->device != 0x7110)) continue;
            unsigned pirq = (p->dev + p->irq_pin - 2) & 3;
            unsigned route = (audio_pci_read(io, r, 0x60) >> (pirq * 8)) & 255;
            return route == p->irq_line;
        }
    }
    for (uint32_t address = 0xf0000; address <= 0x100000 - 48; address += 16)
        if (audio_pir_route(io, p, P2V(address), 0x100000 - address)) return true;
    return false;
}
