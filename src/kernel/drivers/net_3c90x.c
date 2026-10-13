/* 3Com 905/905B/905C copper: windowed registers, UPD/DPD DMA.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "net_dma.h"
#define XL_IRQS 0x606u
static const struct nic_mii xl_mii = {8,2,1,2,2,4,0};
static bool xl_match(const struct pci_func *p)
{
    return p->vendor == 0x10b7 && (p->device == 0x9050 || p->device == 0x9055 || p->device == 0x9200) && p->class_code == 2 && !p->subclass;
}
static int xl_wait(struct netif *d)
{
    for (unsigned i = 0; i < 100; i++) {
        unsigned s = net_read(d, 0x0e, 2); if (s == 0xffff) return -5;
        if (!(s & 0x1000)) return 0;
        d->io.sleep(d->io.cookie, 1);
    }
    return -110;
}
static int xl_command(struct netif *d, unsigned cmd)
{
    int err = xl_wait(d); if (err) return err;
    net_write(d, 0x0e, cmd, 2); return xl_wait(d);
}
static void xl_window(struct netif *d, unsigned win) { net_write(d, 0x0e, 0x800 | win, 2); }
static bool xl_idle(struct netif *d)
{
    unsigned s = net_read(d, 0x0e, 2); if (s == 0xffff || (s & 0x1000)) return false;
    if (net_read(d, 0x24, 4) || net_read(d, 0x38, 4) || (net_read(d, 0x20, 4) & 0x80)) return false;
    unsigned window = s >> 13; xl_window(d, 4);
    unsigned diag = net_read(d, 6, 2); xl_window(d, window);
    return diag != 0xffff && !(diag & 0x0e00); /* transmit in progress/RX enabled/TX enabled */
}
static void xl_irq_enable(struct netif *d, bool on)
{ net_write(d, 0x0e, 0x7000 | (on ? XL_IRQS : 0), 2); }
static bool xl_interrupt(struct netif *d)
{
    unsigned s = net_read(d, 0x0e, 2);
    if (s == 0xffff) { xl_irq_enable(d, false); d->fault_pending = true; return true; }
    if (!(s & XL_IRQS)) return false;
    xl_irq_enable(d, false); d->pending |= s & XL_IRQS;
    /* TX complete is acknowledged only by popping TxStatus in the worker. */
    net_write(d, 0x0e, 0x6800 | (s & 0x600) | 1, 2); return true;
}
static int xl_ee_word(struct netif *d, unsigned addr, unsigned *value)
{
    net_write(d, 0x0a, 0x80 | addr, 2);
    for (unsigned i = 0; i < 100; i++) {
        unsigned s = net_read(d, 0x0a, 2); if (s == 0xffff) return -5;
        if (!(s & 0x8000)) { *value = net_read(d, 0x0c, 2); return 0; }
        d->io.sleep(d->io.cookie, 1);
    }
    return -110;
}
static int xl_init(struct netif *d)
{
    xl_irq_enable(d, false); int err = xl_command(d, 0); if (err) return err;
    xl_irq_enable(d, false); net_write(d, 0x0e, 0x7800, 2); /* indications disabled */
    xl_window(d, 0);
    for (unsigned i = 0; i < 3; i++) {
        unsigned value; if ((err = xl_ee_word(d, 10 + i, &value))) return err;
        d->address[i * 2] = value >> 8; d->address[i * 2 + 1] = value;
    }
    xl_window(d, 2);
    for (unsigned i = 0; i < 6; i++) { net_write(d, i, d->address[i], 1); net_write(d, 6 + i, 0, 1); }
    xl_window(d, 3); uint32_t config = net_read(d, 0, 4);
    if (config == 0xffffffff) return -5;
    unsigned port = d->pci.device == 0x9050 ? 6 : 8; /* external MII / internal autoneg */
    net_write(d, 0, (config & ~0x00f00000u) | (port << 20), 4);
    net_write(d, 6, net_read(d, 6, 2) & ~0x20u, 2); /* half duplex */
    xl_window(d, 4);
    if ((err = nic_mii_find(d, &xl_mii)) || (err = nic_mii_half(d, &xl_mii))) return err;
    for (unsigned i = 0; i < NIC_RX_COUNT; i++) {
        struct nic_desc *r = nic_desc_at(d, false, i);
        r->word[0] = d->dma_phys + ((i + 1) % NIC_RX_COUNT) * 16;
        r->word[2] = nic_buffer_phys(d, false, i); r->word[3] = NIC_OWN | 1536;
    }
    struct nic_desc *t = nic_desc_at(d, true, 0); t->word[2] = nic_buffer_phys(d, true, 0);
    if (d->pci.device != 0x9050) { net_write(d, 0x2d, 0, 1); net_write(d, 0x3d, 0, 1); } /* disable list polling */
    if ((err = xl_command(d, 0x8005)) || (err = xl_command(d, 0x9fff))) return err; /* station/bcast, whole TX */
    net_write(d, 0x0e, 0x7800 | XL_IRQS, 2); net_write(d, 0x0e, 0x6800 | 0x601, 2);
    net_dma_barrier(); net_write(d, 0x38, d->dma_phys, 4); net_write(d, 0x24, 0, 4); return 0;
}
static int xl_start(struct netif *d)
{
    int err = xl_command(d, 0x2000); if (!err) err = xl_command(d, 0x4800);
    if (!err) err = xl_command(d, 0x3001); /* unstall upload */
    return err;
}
static int xl_stop(struct netif *d)
{
    xl_irq_enable(d, false);
    int err = xl_command(d, 0x1800);
    if (!err) err = xl_command(d, 0x5000);
    if (!err) err = xl_command(d, 0x3000);
    if (!err) err = xl_command(d, 0x3002);
    if (!err) err = xl_command(d, 0);
    if (!err && !xl_idle(d)) err = -5;
    return err;
}
static int xl_transmit(struct netif *d, const uint8_t *data, unsigned len)
{
    if (d->tx_used[0]) return -16;
    if (net_read(d, 0x24, 4)) return -5;
    struct nic_desc *t = nic_desc_at(d, true, 0);
    len = nic_tx_copy(d, 0, data, len); t->word[0] = 0; t->word[1] = 0x8000 | len; /* txIndicate */
    t->word[2] = nic_buffer_phys(d, true, 0); t->word[3] = NIC_OWN | len;
    net_dma_barrier(); net_write(d, 0x24, d->dma_phys + NIC_TX_DESC, 4); return 0;
}
static int xl_poll(struct netif *d, uint8_t *out)
{
    unsigned s = net_read(d, 0x0e, 2), cause = s | net_take_pending(d);
    net_write(d, 0x0e, 0x6800 | (s & 0x600) | 1, 2);
    if (s == 0xffff || (cause & 2)) return -5;
    if (d->tx_used[0]) {
        struct nic_desc *t = nic_desc_at(d, true, 0);
        if (t->word[0] || t->word[2] != nic_buffer_phys(d, true, 0) || t->word[3] != (NIC_OWN | d->tx_length[0])) return -5;
        unsigned status = net_read(d, 0x1b, 1);
        if (status == 0xff) return -5;
        if (status & 0x80) {
            net_write(d, 0x1b, 0, 1); /* pop completed transmission, not just DMA */
            if (net_read(d, 0x24, 4)) return -5;
            if (status & 0x36) { d->stats.tx_errors++; return -5; }
            net_tx_complete(d, 0, !(status & 8));
            if ((status & 8) && xl_command(d, 0x4800)) return -5;
        } else if (net_expired(d, d->tx_deadline[0])) { d->stats.tx_errors++; return -110; }
    }
    net_dma_barrier(); struct nic_desc *r = nic_desc_at(d, false, d->rx_head); s = r->word[1];
    if (!(s & 0x8000)) return 0;
    unsigned len = s & 0x1fff;
    if (r->word[0] != d->dma_phys + ((d->rx_head + 1) % NIC_RX_COUNT) * 16 ||
        r->word[2] != nic_buffer_phys(d, false, d->rx_head) || r->word[3] != (NIC_OWN | 1536) || len < 14 || len > NET_FRAME_MAX) {
        d->stats.rx_errors++; return -5;
    }
    bool good = !(s & 0x4000);
    if (good) memcpy(out, nic_buffer(d, false, d->rx_head), len); else d->stats.rx_errors++;
    net_dma_barrier(); r->word[1] = 0; net_dma_barrier();
    int err = xl_command(d, 0x3001); if (err) return err;
    d->rx_head = (d->rx_head + 1) % NIC_RX_COUNT; return good ? (int)len : 0;
}
static enum net_link xl_link(struct netif *d)
{ xl_window(d, 4); return nic_mii_link(d, &xl_mii); }
const struct netif_driver net_3c90x = {
    "3c905", xl_match, 128, 0, NIC_DMA_PAGES, xl_idle, xl_init, xl_start, xl_stop,
    xl_transmit, xl_poll, xl_link, nic_mac, xl_interrupt, xl_irq_enable
};
