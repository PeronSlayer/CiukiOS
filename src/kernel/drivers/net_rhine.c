/* VIA Rhine I/II/III, chained descriptors and bounded hardware MII.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "net_dma.h"
#define RH_IRQS 0x5c6fu
static const struct nic_mii rh_serial_mii = {0x70,1,1,4,2,0x18,0x10};
static bool rh_match(const struct pci_func *p)
{
    return p->vendor == 0x1106 && (p->device == 0x3043 || p->device == 0x3065 || p->device == 0x3106) && p->class_code == 2 && !p->subclass;
}
static bool rh_idle(struct netif *d)
{ unsigned c = net_read(d, 8, 2); return c != 0xffff && !(c & 0x801b); }
static void rh_irq_enable(struct netif *d, bool on) { net_write(d, 0x0e, on ? RH_IRQS : 0, 2); }
static bool rh_interrupt(struct netif *d)
{
    unsigned s = net_read(d, 0x0c, 2);
    if (s == 0xffff) { rh_irq_enable(d, false); d->fault_pending = true; return true; }
    if (!(s & RH_IRQS)) return false;
    rh_irq_enable(d, false); d->pending |= s & RH_IRQS; net_write(d, 0x0c, s & RH_IRQS, 2); return true;
}
static int rh_mii(struct netif *d, unsigned reg, uint16_t *value, bool write)
{
    /* Rhine I lacks Rhine II's MIDLE handshake. Its documented direct
     * programming mode avoids the automatic-polling command interface. */
    if (d->pci.device == 0x3043)
        return write ? nic_mii_write(d, &rh_serial_mii, d->phy, reg, *value) :
                       nic_mii_read(d, &rh_serial_mii, d->phy, reg, value);
    net_write(d, 0x70, 0, 1); /* disable automatic polling before software command */
    for (unsigned i = 0; !(net_read(d, 0x71, 1) & 0x80); i++) {
        if (i == 100) return -110;
        if (d->io.delay(d->io.cookie, 10)) return -110;
    }
    net_write(d, 0x6c, d->phy, 1);
    net_write(d, 0x71, reg, 1);
    if (write) net_write(d, 0x72, *value, 2);
    unsigned command = write ? 0x20 : 0x40; net_write(d, 0x70, command, 1);
    for (unsigned i = 0; i < 100; i++) {
        unsigned s = net_read(d, 0x70, 1); if (s == 0xff) return -5;
        if (!(s & command)) { if (!write) *value = net_read(d, 0x72, 2); return 0; }
        if (d->io.delay(d->io.cookie, 10)) return -110;
    }
    return -110;
}
static int rh_reset(struct netif *d)
{
    net_write(d, 9, 0x80, 1);
    for (unsigned i = 0; i < 100; i++) {
        unsigned c = net_read(d, 9, 1); if (c == 0xff) return -5;
        if (!(c & 0x80)) return rh_idle(d) ? 0 : -5;
        d->io.sleep(d->io.cookie, 1);
    }
    return -110;
}
static int rh_init(struct netif *d)
{
    rh_irq_enable(d, false); int err = rh_reset(d); if (err) return err;
    rh_irq_enable(d, false); net_write(d, 0x0c, 0xffff, 2);
    for (unsigned i = 0; i < 6; i++) d->address[i] = net_read(d, i, 1);
    d->phy = net_read(d, 0x6c, 1) & 31;
    if (d->pci.device == 0x3106) d->phy = 1; /* integrated Rhine III PHY */
    uint16_t v = 0x00a1; if ((err = rh_mii(d, 4, &v, true))) return err;
    v = 0x1200; if ((err = rh_mii(d, 0, &v, true))) return err;
    net_write(d, 9, 8, 1); /* half duplex, explicit TX demand, no early modes */
    net_write(d, 6, 0x0c | 0x60, 1); /* station+broadcast, no runt/error/promiscuous */
    net_write(d, 7, 0x80, 1); /* store and forward; CRC enabled */
    net_write(d, 0x10, 0, 4); net_write(d, 0x14, 0, 4);
    for (unsigned i = 0; i < NIC_RX_COUNT; i++) {
        struct nic_desc *r = nic_desc_at(d, false, i); r->word[1] = 0x8000 | 1536;
        r->word[2] = nic_buffer_phys(d, false, i); r->word[3] = d->dma_phys + ((i + 1) % NIC_RX_COUNT) * 16;
        r->word[0] = NIC_OWN;
    }
    for (unsigned i = 0; i < NIC_TX_COUNT; i++) {
        struct nic_desc *t = nic_desc_at(d, true, i); t->word[2] = nic_buffer_phys(d, true, i);
        t->word[3] = d->dma_phys + NIC_TX_DESC + ((i + 1) % NIC_TX_COUNT) * 16;
    }
    net_dma_barrier(); net_write(d, 0x18, d->dma_phys, 4); net_write(d, 0x1c, d->dma_phys + NIC_TX_DESC, 4); return 0;
}
static int rh_start(struct netif *d)
{ net_write(d, 8, 0x1a, 1); return (net_read(d, 8, 1) & 0x1a) == 0x1a ? 0 : -5; }
static int rh_stop(struct netif *d)
{
    rh_irq_enable(d, false); net_write(d, 8, 4, 1);
    for (unsigned i = 0; i < 100; i++) {
        if (rh_idle(d)) {
            int err = rh_reset(d); if (err) return err;
            net_write(d, 0x18, 0, 4); net_write(d, 0x1c, 0, 4); return 0;
        }
        d->io.sleep(d->io.cookie, 1);
    }
    return -110;
}
static bool rh_descriptor(struct netif *d, bool tx, unsigned i)
{
    struct nic_desc *r = nic_desc_at(d, tx, i);
    return r->word[2] == nic_buffer_phys(d, tx, i) && r->word[3] == d->dma_phys +
        (tx ? NIC_TX_DESC : NIC_RX_DESC) + ((i + 1) % (tx ? NIC_TX_COUNT : NIC_RX_COUNT)) * 16;
}
static int rh_transmit(struct netif *d, const uint8_t *data, unsigned len)
{
    unsigned i = d->tx_head; if (d->tx_used[i]) return -16;
    struct nic_desc *t = nic_desc_at(d, true, i);
    if (!rh_descriptor(d, true, i) || (t->word[0] & NIC_OWN)) return -5;
    len = nic_tx_copy(d, i, data, len); t->word[1] = 0x00e08000 | len;
    net_dma_barrier(); t->word[0] = NIC_OWN; net_dma_barrier();
    net_write(d, 8, 0x3a, 1); d->tx_head = (i + 1) % NIC_TX_COUNT; return 0;
}
static int rh_poll(struct netif *d, uint8_t *out)
{
    unsigned s = net_read(d, 0x0c, 2), cause = s | net_take_pending(d); net_write(d, 0x0c, s & RH_IRQS, 2);
    if (s == 0xffff || (cause & (0x40 | 0x210 | 0x2000))) return -5;
    if (cause & 0x1800) d->stats.rx_dropped++;
    net_dma_barrier();
    for (unsigned i = 0; i < NIC_TX_COUNT; i++) if (d->tx_used[i]) {
        struct nic_desc *t = nic_desc_at(d, true, i); if (!rh_descriptor(d, true, i)) return -5;
        if (!(t->word[0] & NIC_OWN)) {
            if (t->word[0] & 0x3900) return -5;
            net_tx_complete(d, i, !(t->word[0] & 0x8000));
        } else if (net_expired(d, d->tx_deadline[i])) { d->stats.tx_errors++; return -110; }
    }
    struct nic_desc *r = nic_desc_at(d, false, d->rx_head); s = r->word[0];
    if (s & NIC_OWN) return 0;
    unsigned bytes = (s >> 16) & 0x7ff;
    if (!rh_descriptor(d, false, d->rx_head) || r->word[1] != (0x8000 | 1536) ||
        (s & 0x78000000) || bytes < 18 || bytes > NET_FRAME_MAX + 4) { d->stats.rx_errors++; return -5; }
    bool good = (s & 0x83ff) == 0x8300;
    if (good) memcpy(out, nic_buffer(d, false, d->rx_head), bytes - 4); else d->stats.rx_errors++;
    net_dma_barrier(); r->word[0] = NIC_OWN; net_dma_barrier(); net_write(d, 8, 0x5a, 1);
    d->rx_head = (d->rx_head + 1) % NIC_RX_COUNT; return good ? (int)bytes - 4 : 0;
}
static enum net_link rh_link(struct netif *d)
{
    uint16_t v; if (rh_mii(d, 1, &v, false) || rh_mii(d, 1, &v, false) || v == 0xffff) {
        d->fault_pending = true; return NET_LINK_UNKNOWN;
    }
    return v & 4 ? NET_LINK_UP : NET_LINK_DOWN;
}
const struct netif_driver net_rhine = {
    "rhine", rh_match, 256, 0, NIC_DMA_PAGES, rh_idle, rh_init, rh_start, rh_stop,
    rh_transmit, rh_poll, rh_link, nic_mac, rh_interrupt, rh_irq_enable, 0
};
