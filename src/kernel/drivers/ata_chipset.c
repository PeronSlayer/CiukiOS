/* Boot-only ATA chipset adapter; no foreign driver framework.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/ata.h>
#include <ciuki/task.h>

static uint8_t ata_cfg_read(void *ctx, const struct pci_func *p, unsigned reg)
{
    (void)ctx;
    uint32_t f = irq_save();
    outl(0xcf8, 0x80000000u | (uint32_t)p->bus << 16 | p->dev << 11 | p->fn << 8 | (reg & 0xfcu));
    uint8_t v = (uint8_t)(inl(0xcfc) >> (8 * (reg & 3)));
    irq_restore(f);
    return v;
}
static void ata_cfg_write(void *ctx, const struct pci_func *p, unsigned reg, uint8_t v)
{
    (void)ctx;
    uint32_t f = irq_save();
    outl(0xcf8, 0x80000000u | (uint32_t)p->bus << 16 | p->dev << 11 | p->fn << 8 | (reg & 0xfcu));
    outb((uint16_t)(0xcfc + (reg & 3)), v); /* byte access avoids PCI Status W1C */
    irq_restore(f);
}
const struct ata_config_ops ata_native_config = { ata_cfg_read, ata_cfg_write };

int ata_pci_resources(const struct pci_func *p, unsigned ch, bool routed, struct ata_resources *r)
{
    if (ata_addin_match(p)) return ata_addin_resources(p, ch, routed, r);
    if (!p || !r || ch >= 2 || p->class_code != 1 || p->subclass != 1 ||
        (p->header_type & 0x7f) || !(p->command & 1)) return -FS_EINVAL;
    if (p->vendor == 0x8086 && (p->device == 0x248a || p->device == 0x248b) && (p->prog_if & 5))
        return -FS_EOPNOTSUPP; /* ICH3 erratum 18 covers the entire function */
    struct ata_resources v = { .base = ch ? 0x170 : 0x1f0,
        .control = ch ? 0x376 : 0x3f6, .irq = ch ? 15 : 14 };
    v.native_irq = !!(p->prog_if & (1u << (ch * 2)));
    if (v.native_irq) {
        if (!routed || (p->command & 0x400) || !p->irq_pin || p->irq_pin > 4 || p->irq_line < 3 ||
            p->irq_line > 15 || p->irq_line == 8 || p->irq_line == 13)
            return -FS_EOPNOTSUPP;
        uint32_t b = p->bar[ch * 2], ctl = p->bar[ch * 2 + 1];
        if (!(b & 1) || !(ctl & 1) || (ctl & 2) || !(b & ~3u) || !(ctl & ~3u) ||
            (b & ~3u) > 0xfff8 || (b & 6) || (ctl & ~3u) > 0xfffc)
            return -FS_EINVAL;
        v.base = (uint16_t)(b & ~3u); v.control = (uint16_t)((ctl & ~3u) + 2);
        v.irq = p->irq_line;
    }
    if (p->prog_if & 0x80) {
        uint32_t b = p->bar[4];
        if (!(b & 1) || !(b & ~3u) || (b & 14) || (b & ~3u) > 0xfff0) return -FS_EINVAL;
        v.bm = (uint16_t)((b & ~3u) + ch * 8);
    }
    /* Validate all occupied extents, including the native control BAR's
     * four decoded ports, even though only +2 is used. */
    unsigned ctlbase = v.native_irq ? v.control - 2u : v.control;
    unsigned ctlend = ctlbase + (v.native_irq ? 4u : 1u);
    unsigned bm = v.bm ? v.bm - ch * 8u : 0;
    if ((v.base < ctlend && ctlbase < v.base + 8u) ||
        (bm && ((v.base < bm + 16 && bm < v.base + 8u) ||
                (ctlbase < bm + 16 && bm < ctlend)))) return -FS_EINVAL;
    if (v.native_irq && !v.bm) return -FS_EOPNOTSUPP; /* shared IRQ source proof */
    /* Reject cross-channel aliases before either channel probes hardware. */
    unsigned peer = ch ^ 1u;
    bool peer_native = !!(p->prog_if & (1u << (peer * 2)));
    unsigned peerbase = peer ? 0x170 : 0x1f0, peerctl = peer ? 0x376 : 0x3f6;
    unsigned peerctlsize = 1;
    if (peer_native) {
        uint32_t b = p->bar[peer * 2], ctl = p->bar[peer * 2 + 1];
        if (!(b & 1) || !(ctl & 1) || (ctl & 2) || !(b & ~3u) || !(ctl & ~3u) ||
            (b & ~3u) > 0xfff8 || (b & 6) || (ctl & ~3u) > 0xfffc) return -FS_EINVAL;
        peerbase = b & ~3u; peerctl = ctl & ~3u; peerctlsize = 4;
    }
    unsigned starts[] = {v.base, ctlbase, peerbase, peerctl, bm};
    unsigned sizes[] = {8, ctlend - ctlbase, 8, peerctlsize, bm ? 16u : 0};
    for (unsigned i = 0; i < 5; i++) for (unsigned j = i + 1; j < 5; j++)
        if (sizes[i] && sizes[j] && starts[i] < starts[j] + sizes[j] && starts[j] < starts[i] + sizes[i])
            return -FS_EINVAL;
    *r = v;
    return 0;
}

unsigned ata_chip_read(const struct ata_chipset *s, unsigned reg, unsigned bytes)
{
    unsigned v = 0;
    for (unsigned i = 0; i < bytes; i++) v |= (unsigned)s->ops->read(s->ctx, s->pci, reg + i) << (8 * i);
    return v;
}
const struct pci_func *ata_chip_bridge(const struct ata_chipset *s, uint16_t vendor, uint16_t id)
{
    const struct pci_func *found = 0;
    for (unsigned i = 0; i < pci_count(); i++) {
        const struct pci_func *p = pci_get(i);
        if (p->vendor != vendor || p->device != id || p->bus != s->pci->bus || p->class_code != 6) continue;
        if (found) return 0;
        found = p;
    }
    return found;
}
void ata_chipset_init(struct ata_chipset *s, const struct pci_func *p, unsigned ch,
                      const struct ata_config_ops *ops, void *ctx)
{
    *s = (struct ata_chipset){ .pci = p, .channel = ch, .ops = ops, .ctx = ctx,
        .maximum = -1, .reason = "unsupported_chipset" };
    if (!p || ch >= 2 || !ops || !ops->read || !ops->write) return;
    switch (p->vendor) {
    case 0x105a: case 0x1103: ata_addin_detect(s); break;
    case 0x8086: ata_piix_detect(s); break;
    case 0x1106: ata_via_detect(s); break;
    case 0x1039: ata_sis_detect(s); break;
    case 0x10b9: ata_ali_detect(s); break;
    case 0x10de: ata_nv_detect(s); break;
    }
}
bool ata_plan_add(struct ata_timing_plan *p, unsigned reg, unsigned mask, unsigned v)
{
    if (p->count >= ATA_TIMING_WRITES || reg > 255 || !mask || mask > 255 || (v & ~mask)) return false;
    p->writes[p->count++] = (struct ata_timing_write){ (uint8_t)reg, (uint8_t)mask, (uint8_t)v };
    return true;
}
int ata_chipset_plan(struct ata_chipset *s, unsigned unit, const uint8_t id[512], struct ata_timing_plan *p)
{
    if (!s || !id || !p || unit >= 2) return -1;
    p->count = 0;
    if (s->maximum < 0 || s->maximum > 6) return -1;
    s->reason = "identify_no_udma";
    if (!(fs_rd16(id + 98) & 0x100) || !(fs_rd16(id + 106) & 4) || (fs_rd16(id) & 0x8000)) return -1;
    unsigned supported = fs_rd16(id + 176) & 0x7f;
    int mode = s->maximum;
    unsigned cable = fs_rd16(id + 186);
    if (!s->cable80 || (cable & 0xe000) != 0x6000) { if (mode > 2) mode = 2; }
    if (s->family == ATA_CHIP_NV) {
        static const int limit[] = { 2, 1, 0, -1, 3, 4, 5, 6 };
        unsigned saved = s->saved_udma[unit];
        int ceiling = (saved & 0xc0) == 0xc0 ? limit[saved & 7] : 2;
        if (ceiling < 2) ceiling = 2;
        if (mode > ceiling) mode = ceiling;
    }
    if (s->family == ATA_CHIP_SIS && s->layout == 3) {
        unsigned base = (ata_chip_read(s, 0x54, 4) & 0x40000000) ? 0x70 : 0x40;
        if (!(ata_chip_read(s, base + 8 * s->channel + 4 * unit, 1) & 8) && mode > 5) mode = 5;
    }
    if (s->family == ATA_CHIP_VIA && s->bridge && s->bridge->device == 0x0586) {
        const char bad[] = "TS64GSSD25-M";
        bool match = true;
        for (unsigned i = 0; i < 40; i++) {
            unsigned ch = id[54 + (i ^ 1)];
            if (i < sizeof(bad) - 1 ? ch != (unsigned)bad[i] : ch != ' ' && ch != 0) match = false;
        }
        if (match) { s->reason = "via_ssd_erratum"; return -1; }
    }
    while (mode >= 0 && (supported & ((1u << (mode + 1)) - 1)) != ((1u << (mode + 1)) - 1)) mode--;
    if (s->family >= ATA_CHIP_PDC_OLD) mode = ata_addin_limit(s, unit, id, mode);
    if (mode < 0) return -1;
    bool ok = false;
    switch (s->family) {
    case ATA_CHIP_PDC_OLD: case ATA_CHIP_PDC_NEW: case ATA_CHIP_HPT36:
    case ATA_CHIP_HPT37: case ATA_CHIP_HPTN: ok = ata_addin_plan(s, unit, (unsigned)mode, p); break;
    case ATA_CHIP_PIIX: ok = ata_piix_plan(s, unit, (unsigned)mode, p); break;
    case ATA_CHIP_VIA: ok = ata_via_plan(s, unit, (unsigned)mode, p); break;
    case ATA_CHIP_SIS: ok = ata_sis_plan(s, unit, (unsigned)mode, p); break;
    case ATA_CHIP_ALI: ok = ata_ali_plan(s, unit, (unsigned)mode, p); break;
    case ATA_CHIP_NV: ok = ata_nv_plan(s, unit, (unsigned)mode, p); break;
    default: break;
    }
    s->reason = ok ? "negotiated" : "timing_plan_refused";
    return ok ? mode : -1;
}
int ata_chipset_program(struct ata_channel *c, const struct ata_timing_plan *p)
{
    /* Promise MMIO timing is repaired after hardware SET FEATURES autotiming. */
    if (c && c->chipset.family == ATA_CHIP_PDC_NEW && p && !p->count)
        return ata_addin_idle(c) && !c->modes_published ? 0 : -FS_EBUSY;
    if (!c || !p || !p->count || p->count > ATA_TIMING_WRITES || !c->chipset.pci ||
        !c->chipset.ops || !c->dma.ops || c->mutex.owner != g_current || !g_current ||
        c->active || c->dma.armed || c->dma.started || c->stopped || c->quiescing || c->modes_published)
        return -FS_EBUSY;
    const struct ata_chipset *s = &c->chipset;
    uint8_t drive_status = c->ops->in8(c->port_ctx, c->control);
    if (!drive_status || drive_status == 0xff || (drive_status & 0x88)) return -FS_EBUSY;
    /* Both engines must be idle before touching shared PCI timing fields. */
    uint16_t base = (uint16_t)(c->dma.base - s->channel * 8);
    unsigned engines=ata_addin_match(s->pci) ? ata_addin_channels(s->pci) : 2;
    for (unsigned i = 0; i < engines; i++) {
        unsigned port = base + i * 8;
        if ((c->dma.ops->in8(c->dma.ctx, (uint16_t)port) & 1) ||
            (c->dma.ops->in8(c->dma.ctx, (uint16_t)(port + 2)) & ATA_BM_ACTIVE)) return -FS_EBUSY;
    }
    for (unsigned i = 0; i < p->count; i++) {
        const struct ata_timing_write *w = &p->writes[i];
        uint8_t before = s->ops->read(s->ctx, s->pci, w->reg);
        uint8_t v = (uint8_t)((before & ~w->mask) | w->value);
        s->ops->write(s->ctx, s->pci, w->reg, v);
        if (s->ops->read(s->ctx, s->pci, w->reg) != v) return -FS_EIO;
    }
    return 0;
}
