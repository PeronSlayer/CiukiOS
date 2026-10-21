/* Intel 82540/82545 copper, legacy descriptors and uncached BAR0 MMIO.
 * Manufacturer references: docs/validation/f6-08-nic-drivers.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "net_dma.h"
#define E1_IRQS 0xd5u /* TXDW, LSC, RXDMT0, RXO, RXT0 */
struct e1_rx { uint32_t low, high; volatile uint16_t length, checksum; volatile uint8_t status, errors; uint16_t special; };
struct e1_tx { uint32_t low, high; uint16_t length; uint8_t checksum, command; volatile uint8_t status; uint8_t start; uint16_t special; };
_Static_assert(sizeof(struct e1_rx) == 16 && sizeof(struct e1_tx) == 16, "8254x descriptors");
static uint32_t e1_read(struct netif *d, unsigned reg)
{ return net_read(d, reg, 4); }
static void e1_write(struct netif *d, unsigned reg, uint32_t value)
{ net_write(d, reg, value, 4); }
static bool e1_match(const struct pci_func *p)
{
    if (p->vendor != 0x8086 || p->class_code != 2 || p->subclass) return false;
    return p->device == 0x100e || p->device == 0x1015 || p->device == 0x1016 ||
           p->device == 0x1017 || p->device == 0x100f || p->device == 0x1026;
}
static bool e1_idle(struct netif *d)
{
    uint32_t rx = e1_read(d, 0x100), tx = e1_read(d, 0x400), ctrl = e1_read(d, 0);
    return rx != 0xffffffff && tx != 0xffffffff && ctrl != 0xffffffff &&
           !(rx & 2) && !(tx & 2) && !(ctrl & (1u << 26));
}
static void e1_irq_enable(struct netif *d, bool on)
{ e1_write(d, on ? 0xd0 : 0xd8, on ? E1_IRQS : 0xffffffff); }
static bool e1_interrupt(struct netif *d)
{
    uint32_t status = e1_read(d, 0xc0); /* read clears causes */
    if (status == 0xffffffff) { e1_irq_enable(d, false); d->fault_pending = true; return true; }
    if (!(status & E1_IRQS)) return false;
    e1_irq_enable(d, false); d->pending |= status & E1_IRQS; return true;
}
static int e1_reset(struct netif *d)
{
    e1_irq_enable(d, false); e1_write(d, 0x100, 0); e1_write(d, 0x400, 8);
    d->io.sleep(d->io.cookie, 10); /* drain outstanding PCI transactions */
    uint32_t ctrl = e1_read(d, 0); if (ctrl == 0xffffffff) return -5;
    e1_write(d, 0, ctrl | (1u << 26));
    for (unsigned n = 0; n < 100; n++) {
        d->io.sleep(d->io.cookie, 1); ctrl = e1_read(d, 0);
        if (ctrl == 0xffffffff) return -5;
        if (!(ctrl & (1u << 26))) {
            d->io.sleep(d->io.cookie, 5); e1_irq_enable(d, false);
            (void)e1_read(d, 0xc0); return e1_idle(d) ? 0 : -5;
        }
    }
    return -110;
}
static int e1_init(struct netif *d)
{
    int err = e1_reset(d); if (err) return err;
    /* CTRL.RST clears RAH.AV. Read the permanent station address from
     * EEPROM, then publish RAL before RAH and explicitly restore AV. */
    for (unsigned i = 0; i < 3; i++) {
        e1_write(d, 0x14, (i << 8) | 1);
        uint32_t word = 0;
        for (unsigned n = 0; n < 20; n++) {
            word = e1_read(d, 0x14); if (word == 0xffffffff) return -5;
            if (word & 0x10) break;
            d->io.sleep(d->io.cookie, 1);
        }
        if (!(word & 0x10)) return -110;
        d->address[i * 2] = word >> 16; d->address[i * 2 + 1] = word >> 24;
    }
    uint32_t low = d->address[0] | (uint32_t)d->address[1] << 8 |
        (uint32_t)d->address[2] << 16 | (uint32_t)d->address[3] << 24;
    uint32_t high = d->address[4] | (uint32_t)d->address[5] << 8;
    e1_write(d, 0x5400, low); e1_write(d, 0x5404, high | NIC_OWN);
    for (unsigned i = 1; i < 16; i++) { e1_write(d, 0x5404 + i * 8, 0); e1_write(d, 0x5400 + i * 8, 0); }
    for (unsigned i = 0; i < 128; i++) e1_write(d, 0x5200 + i * 4, 0);
    /* Eight RX/TX descriptors: hardware ring length is a multiple of 128.
     * TX has one outstanding frame, rotating through all eight entries. */
    for (unsigned i = 0; i < NIC_RX_COUNT; i++) {
        struct e1_rx *r = (void *)nic_desc_at(d, false, i); r->low = nic_buffer_phys(d, false, i);
    }
    e1_write(d, 0x2800, d->dma_phys); e1_write(d, 0x2804, 0); e1_write(d, 0x2808, 128);
    e1_write(d, 0x2810, 0); e1_write(d, 0x2818, 7); e1_write(d, 0x2820, 0);
    e1_write(d, 0x3800, d->dma_phys + NIC_TX_DESC); e1_write(d, 0x3804, 0); e1_write(d, 0x3808, 128);
    e1_write(d, 0x3810, 0); e1_write(d, 0x3818, 0); e1_write(d, 0x3820, 0);
    e1_write(d, 0x410, 10 | (8u << 10) | (6u << 20));
    e1_write(d, 0x5000, 0); /* checksum offload disabled */
    /* Auto speed/duplex, PHY supplied link, set-link-up; no forced mode. */
    e1_write(d, 0, (e1_read(d, 0) & ~((1u << 11) | (1u << 12) | (3u << 8) | 8u)) | (3u << 5));
    net_dma_barrier(); return 0;
}
static int e1_start(struct netif *d)
{
    e1_write(d, 0x400, 2 | 8 | (15u << 4) | (64u << 12));
    e1_write(d, 0x100, 2 | (1u << 15) | (1u << 26)); /* broadcast, strip CRC, 2 KiB */
    return (e1_read(d, 0x100) & 2) && (e1_read(d, 0x400) & 2) ? 0 : -5;
}
static int e1_stop(struct netif *d)
{
    int err = e1_reset(d); if (err) return err;
    e1_write(d, 0x2800, 0); e1_write(d, 0x3800, 0); return 0;
}
static int e1_transmit(struct netif *d, const uint8_t *data, unsigned len)
{
    if (d->tx_used[0]) return -16;
    struct e1_tx *t = (void *)nic_desc_at(d, true, d->tx_head);
    memset(t, 0, sizeof(*t)); t->low = nic_buffer_phys(d, true, 0);
    t->length = nic_tx_copy(d, 0, data, len); t->command = 0x0b; /* EOP, IFCS, RS */
    net_dma_barrier(); e1_write(d, 0x3818, (d->tx_head + 1) % 8); return 0;
}
static int e1_poll(struct netif *d, uint8_t *out)
{
    uint32_t cause = e1_read(d, 0xc0) | net_take_pending(d);
    if (cause == 0xffffffff || (cause & 0x40)) return -5;
    net_dma_barrier();
    if (d->tx_used[0]) {
        struct e1_tx *t = (void *)nic_desc_at(d, true, d->tx_head);
        if (t->low != nic_buffer_phys(d, true, 0) || t->high || t->length != d->tx_length[0]) return -5;
        if (t->status & 1) { net_tx_complete(d, 0, !(t->status & 0x0e)); d->tx_head = (d->tx_head + 1) % 8; }
        else if (net_expired(d, d->tx_deadline[0])) { d->stats.tx_errors++; return -110; }
    }
    struct e1_rx *r = (void *)nic_desc_at(d, false, d->rx_head);
    if (!(r->status & 1)) return 0;
    unsigned len = r->length;
    if (r->low != nic_buffer_phys(d, false, d->rx_head) || r->high || len < 14 || len > NET_FRAME_MAX) {
        d->stats.rx_errors++; return -5;
    }
    bool good = (r->status & 2) && !r->errors;
    if (good) memcpy(out, nic_buffer(d, false, d->rx_head), len); else d->stats.rx_errors++;
    r->length = r->checksum = 0; r->errors = r->status = 0;
    net_dma_barrier(); e1_write(d, 0x2818, d->rx_head); d->rx_head = (d->rx_head + 1) % 8;
    return good ? (int)len : 0;
}
static enum net_link e1_link(struct netif *d)
{
    uint32_t status = e1_read(d, 8);
    if (status == 0xffffffff) { d->fault_pending = true; return NET_LINK_UNKNOWN; }
    return status & 2 ? NET_LINK_UP : NET_LINK_DOWN;
}
static int e1_promiscuous(struct netif *d, bool on)
{
    uint32_t config = e1_read(d, 0x100);
    if (config == UINT32_MAX) return -5;
    config = on ? config | (1u << 3) : config & ~(1u << 3);
    e1_write(d, 0x100, config);
    return e1_read(d, 0x100) == config ? 0 : -5;
}
const struct netif_driver net_e1000 = {
    "e1000", e1_match, 0x20000, 0, NIC_DMA_PAGES, e1_idle, e1_init, e1_start, e1_stop,
    e1_transmit, e1_poll, e1_link, nic_mac, e1_interrupt, e1_irq_enable, e1_promiscuous
};
