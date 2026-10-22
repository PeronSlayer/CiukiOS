/* EHCI 1.0 RAM/register walker, independent of the driver's layout constants.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_TEST_EHCI_MODEL_H
#define CIUKI_TEST_EHCI_MODEL_H
struct ehci_model {
    struct usb_host *host;
    unsigned regs[256], config[64], selected, frame;
    uint64_t now, reset_at[USB_PORT_MAX], bulk_ready_at;
    unsigned packets, completions, doorbells, cfg_writes, reg_writes, port_handoffs, cached_qh;
    bool bios_stuck, smi_stuck, os_stuck, reset_stuck, halt_stuck, mask_stuck;
    bool iaa_lost, ass_stuck, freeze, host_error, dma_error, bme_stuck;
    bool deliver_irq, nak, late_ack;
    unsigned inject, short_bytes, bulk_toggles[2], port_speed[USB_PORT_MAX];
    int (*transaction)(struct ehci_model *, unsigned, unsigned, unsigned, unsigned, uint8_t *, unsigned);
};
static uint32_t *ehci_ram(struct ehci_model *e, unsigned address, unsigned bytes)
{
    assert(address >= e->host->dma_phys && address - e->host->dma_phys <= USB_DMA_BYTES - bytes);
    return (uint32_t *)(e->host->dma + address - e->host->dma_phys);
}
static uint32_t ehci_read(void *ctx, unsigned at, unsigned width)
{
    struct ehci_model *e = ctx; assert(at < sizeof(e->regs));
    if (at == 0x2c) return e->frame & 0x3fff;
    if (at == 0x24 && e->host_error) return e->regs[at/4] | 0x10;
    unsigned mask = width == 1 ? 255 : width == 2 ? 65535 : UINT32_MAX;
    return (e->regs[at/4] >> ((at & 3) * 8)) & mask;
}
static void ehci_write(void *ctx, unsigned at, uint32_t v, unsigned width)
{
    struct ehci_model *e = ctx; assert(width == 4 && !(at & 3)); e->reg_writes++;
    assert(!(e->config[0x40/4] & (1u << 16))); /* ownership precedes MMIO writes */
    if (at == 0x24) { e->regs[at/4] &= ~(v & 0x3f); return; }
    if (at == 0x28 && e->mask_stuck) return;
    if (at == 0x20) {
        if ((v & 2) && !e->reset_stuck) {
            assert(e->regs[0x24/4] & 0x1000);
            e->regs[at/4] = 0; e->regs[0x24/4] = 0x1000; e->regs[0x60/4] = 0;
            return;
        }
        if (v & 0x40) e->doorbells++;
        unsigned s = e->regs[0x24/4];
        s = v & 1 ? s & ~0x1000u : e->halt_stuck ? s : s | 0x1000;
        if ((v & 0x21) == 0x21) s |= 0x8000;
        else if (!e->ass_stuck) s &= ~0x8000u;
        if ((v & 0x11) == 0x11) s |= 0x4000;
        else if (!e->halt_stuck) s &= ~0x4000u;
        e->regs[0x24/4] = s;
    }
    if (at >= 0x64 && at < 0x64 + USB_PORT_MAX * 4) {
        unsigned p = (at - 0x64) / 4, old = e->regs[at/4];
        /* RO CCS/line/OC; W1C changes; PE software can only clear. */
        unsigned nv = (old & ~0x007ff1c4u) | (v & 0x007ff1c4);
        nv &= ~(v & 0x2a); if (!(old & 4)) nv &= ~4u;
        if ((v & 0x100) && !(old & 0x100)) { e->selected = p; e->reset_at[p] = e->now; }
        if (!(v & 0x100) && (old & 0x100)) {
            assert(e->now - e->reset_at[p] >= 50);
            if (e->port_speed[p] == 2) nv |= 4;
        }
        if ((v & 0x2000) && !(old & 0x2000)) e->port_handoffs++;
        e->regs[at/4] = nv; return;
    }
    if (at == 0x60 && (v & 1) && !(e->regs[at/4] & 1)) {
        for (unsigned p = 0; p < (e->regs[1] & 15); p++) e->regs[(0x64 + p * 4)/4] &= ~0x2000u;
    }
    e->regs[at/4] = v;
}
static uint32_t ehci_cfg_read(void *ctx, unsigned at, unsigned width)
{
    struct ehci_model *e = ctx;
    unsigned mask = width == 1 ? 255 : width == 2 ? 65535 : UINT32_MAX;
    return (e->config[at/4] >> ((at & 3) * 8)) & mask;
}
static void ehci_cfg_write(void *ctx, unsigned at, uint32_t v, unsigned width)
{
    struct ehci_model *e = ctx; e->cfg_writes++;
    if (at == 4) {
        assert(width == 2);
        if ((v & 4) && !(e->config[1] & 4)) {
            assert(e->host->dma && e->regs[0x34/4] == e->host->dma_phys);
            assert(e->regs[0x38/4] && e->regs[0x30/4] == 0);
        }
        if (e->bme_stuck && !(v & 4)) v |= e->config[1] & 4;
    }
    if (at == 0x40) {
        assert(!(v & (1u << 16)) || (e->config[at/4] & (1u << 16)));
        if (e->os_stuck) v &= ~(1u << 24);
        if (!e->bios_stuck && (v & (1u << 24))) v &= ~(1u << 16);
    }
    if (at == 0x44) v = e->smi_stuck ? 1 : 0; /* SMI enable vs W1C */
    unsigned shift = (at & 3) * 8, mask = width == 1 ? 255 : width == 2 ? 65535 : UINT32_MAX;
    e->config[at/4] = (e->config[at/4] & ~(mask << shift)) | (v & mask) << shift;
}
static void ehci_qh(struct ehci_model *e, unsigned address, bool periodic, unsigned u)
{
    uint32_t *q = ehci_ram(e, address, 68);
    if (periodic && !(q[2] & (1u << u))) return;
    if ((q[4] & 1) || (q[6] & 0x40)) return;
    assert(q[1] & (1u << 14)); /* qTD owns data toggle */
    assert(((q[1] >> 12) & 3) == 2 && !(q[2] & 0x3fffff00)); /* no unsupported TT */
    unsigned tdaddr = q[4] & ~31u;
    uint32_t *td = ehci_ram(e, tdaddr, 52);
    if (!(td[2] & 0x80)) return;
    for (unsigned i = 8; i < 13; i++) assert(td[i] == 0);
    for (unsigned i = 12; i < 17; i++) assert(q[i] == 0);
    unsigned left = (td[2] >> 16) & 0x7fff, mps = (q[1] >> 16) & 0x7ff;
    unsigned n = left < mps ? left : mps, pid = (td[2] >> 8) & 3;
    assert(mps && pid < 3 && (td[3] & 4095) + n <= 4096);
    uint8_t *buffer = (uint8_t *)ehci_ram(e, td[3], n ? n : 1);
    e->packets++;
    if (!periodic && e->late_ack) { e->cached_qh = address; return; }
    int got = e->inject ? -USB_EIO : e->nak ? -USB_EAGAIN : e->transaction(e, q[1] & 127, (q[1] >> 8) & 15,
        pid == 2 ? USB_PID_SETUP : pid == 1 ? USB_PID_IN : USB_PID_OUT, td[2] >> 31, buffer, n);
    if (got == -USB_EAGAIN) return;
    unsigned error = e->inject ? e->inject : got == -USB_EPIPE ? 0x40 : got < 0 ? 0x48 : 0;
    e->inject = 0;
    if (error) { td[2] = (td[2] & ~0x80u) | error | 0x40; q[6] = td[2]; e->regs[0x24/4] |= 2; return; }
    assert(got >= 0 && (unsigned)got <= n);
    left -= (unsigned)got;
    td[2] = (td[2] & ~0x7fff0000u) | (left << 16); td[2] ^= 1u << 31;
    td[3] += (unsigned)got;
    if (e->dma_error) td[2] |= 0x7fff0000;
    q[3] = tdaddr; q[6] = td[2];
    if ((unsigned)got < n || !left) {
        td[2] &= ~0x80u; q[6] = td[2]; q[4] = (unsigned)got < n ? td[1] : td[0];
        e->completions++; e->regs[0x24/4] |= 1;
    }
}
static void ehci_walk(struct ehci_model *e, unsigned link, bool periodic, unsigned u)
{
    unsigned first = link & ~31u;
    for (unsigned n = 0; !(link & 1); n++) {
        assert(n < 66 && (link & 6) == 2);
        unsigned address = link & ~31u;
        uint32_t *q = ehci_ram(e, address, 68);
        unsigned next = q[0]; ehci_qh(e, address, periodic, u); link = next;
        if (!periodic && (link & ~31u) == first) break;
    }
}
static uint64_t ehci_now(void *ctx) { return ((struct ehci_model *)ctx)->now; }
static void ehci_sleep(void *ctx, unsigned ms)
{
    struct ehci_model *e = ctx;
    while (ms--) {
        e->now++;
        for (unsigned u = 0; u < 8; u++) {
            unsigned cmd = e->regs[0x20/4];
            if (!(cmd & 1) || !(e->config[1] & 4)) continue;
            if (!e->freeze) e->frame = (e->frame + 1) & 0x3fff;
            if (cmd & 0x10) {
                unsigned f = (e->frame >> 3) & 1023;
                ehci_walk(e, *ehci_ram(e, e->regs[0x34/4] + f * 4, 4), true, e->frame & 7);
            }
            if (cmd & 0x20) ehci_walk(e, e->regs[0x38/4] | 2, false, u);
            if ((cmd & 0x40) && e->late_ack && e->cached_qh) {
                e->late_ack = false;
                ehci_qh(e, e->cached_qh, false, u); /* ACK fetched before unlink */
            }
            if ((cmd & 0x40) && !e->iaa_lost) { e->regs[0x20/4] &= ~0x40u; e->regs[0x24/4] |= 0x20; }
            if (e->deliver_irq && e->host->irq_attached) (void)usb_host_irq(e->host);
        }
    }
}
static struct usb_io ehci_io(struct ehci_model *e)
{ return (struct usb_io){e,ehci_read,ehci_write,ehci_cfg_read,ehci_cfg_write,ehci_now,ehci_sleep}; }
static void ehci_model_init(struct ehci_model *e, struct usb_host *h)
{
    memset(e, 0, sizeof(*e)); e->host = h;
    e->regs[0] = 0x01000020; e->regs[1] = 2; e->regs[2] = 0x4001;
    e->regs[0x24/4] = 0x1000; e->config[0x40/4] = 1 | (1u << 16);
    e->regs[0x64/4] = e->regs[0x68/4] = 3;
    e->port_speed[0] = e->port_speed[1] = 2;
}
#endif
