/* DEC/Intel 21140/21143: chained descriptors and a perfect setup filter.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "net_dma.h"
#define TU_IRQS 0x1a1efu
#define TU_RUN 0x2002u
#define TU_CHAIN (1u << 24)
static const struct nic_mii tu_mii = {0x48,4,0x10000,0x20000,0x80000,0,0x40000};
static bool tu_match(const struct pci_func *p)
{
    return p->vendor == 0x1011 && (p->device == 0x0009 || p->device == 0x0019) && p->class_code == 2 && !p->subclass;
}
static bool tu_idle(struct netif *d)
{
    uint32_t s = net_read(d, 0x28, 4), c = net_read(d, 0x30, 4);
    return s != 0xffffffff && c != 0xffffffff && !(s & 0x7e0000) && !(c & TU_RUN);
}
static void tu_irq_enable(struct netif *d, bool on) { net_write(d, 0x38, on ? TU_IRQS : 0, 4); }
static bool tu_interrupt(struct netif *d)
{
    uint32_t s = net_read(d, 0x28, 4);
    if (s == 0xffffffff) { tu_irq_enable(d, false); d->fault_pending = true; return true; }
    if (!(s & TU_IRQS)) return false;
    tu_irq_enable(d, false); d->pending |= s & TU_IRQS; net_write(d, 0x28, s & TU_IRQS, 4); return true;
}
static int tu_ee_bit(struct netif *d, unsigned bit, unsigned *sample)
{
    unsigned v = 0x4801 | (bit ? 4 : 0); /* serial-ROM read/select/chip-select */
    net_write(d, 0x48, v, 4); if (d->io.delay(d->io.cookie, 1)) return -110;
    net_write(d, 0x48, v | 2, 4); if (d->io.delay(d->io.cookie, 1)) return -110;
    if (sample) *sample = !!(net_read(d, 0x48, 4) & 8);
    return 0;
}
static int tu_ee_word(struct netif *d, unsigned address, uint16_t *out, bool detect)
{
    net_write(d, 0x48, 0x4800, 4); unsigned bit = 1, value = 0;
    for (int i = 2; i >= 0; i--) if (tu_ee_bit(d, (6u >> i) & 1, 0)) return -110;
    unsigned bits = detect ? 6 : d->eeprom_bits;
    for (int i = (int)bits - 1; i >= 0; i--) if (tu_ee_bit(d, (address >> i) & 1, &bit)) return -110;
    if (detect) {
        /* Probe address zero, extending zeros until the dummy-zero response. */
        while (bit && bits < 8) { bits++; if (tu_ee_bit(d, 0, &bit)) return -110; }
        d->eeprom_bits = bits;
    }
    if (bit) { net_write(d, 0x48, 0x4800, 4); return -5; }
    for (unsigned i = 0; i < 16; i++) {
        if (tu_ee_bit(d, 0, &bit)) return -110;
        value = (value << 1) | bit;
    }
    net_write(d, 0x48, 0x4800, 4); *out = value; return 0;
}
static int tu_wait_idle(struct netif *d)
{
    for (unsigned i = 0; i < 100; i++) {
        if (tu_idle(d)) return 0;
        if (net_read(d, 0x28, 4) == 0xffffffff) return -5;
        d->io.sleep(d->io.cookie, 1);
    }
    return -110;
}
static int tu_init(struct netif *d)
{
    tu_irq_enable(d, false); net_write(d, 0, 1, 4);
    for (unsigned i = 0; ; i++) {
        uint32_t c = net_read(d, 0, 4); if (c == 0xffffffff) return -5;
        if (!(c & 1)) break;
        if (i == 99) return -110;
        d->io.sleep(d->io.cookie, 1);
    }
    tu_irq_enable(d, false); net_write(d, 0x28, TU_IRQS, 4);
    uint16_t word; int err = tu_ee_word(d, 0, &word, true); if (err) return err;
    /* DEC standardized PCI SROM station address bytes 20..25. */
    for (unsigned i = 0; i < 3; i++) {
        if ((err = tu_ee_word(d, 10 + i, &word, false))) return err;
        d->address[i * 2] = word; d->address[i * 2 + 1] = word >> 8;
    }
    err = nic_mii_find(d, &tu_mii);
    if (err == -110) return err;
    if (!err) {
        if ((err = nic_mii_half(d, &tu_mii))) return err;
        d->media_control = (1u << 18) | (1u << 19) | (1u << 21) | (1u << 25);
        if (d->pci.device == 0x0019) {
            net_write(d, 0x68, 0, 4); net_write(d, 0x70, 0, 4); net_write(d, 0x78, 8, 4);
        } /* manufacturer MII half-duplex mode, heartbeat disabled */
    } else {
        if (d->pci.device != 0x0019) return -19;
        /* Built-in 21143 10BASE-T path; non-MII boards need physical
         * qualification. No fabricated link for a missing external PHY. */
        d->media_control = (1u << 21) | (1u << 25);
        net_write(d, 0x68, 0, 4); net_write(d, 0x70, 0x7f3f, 4);
        net_write(d, 0x78, 8, 4); net_write(d, 0x68, 1, 4);
    }
    net_write(d, 0, 16u << 8, 4); /* little endian, bounded PCI burst */
    for (unsigned i = 0; i < NIC_RX_COUNT; i++) {
        struct nic_desc *r = nic_desc_at(d, false, i);
        r->word[1] = TU_CHAIN | 1536; r->word[2] = nic_buffer_phys(d, false, i);
        r->word[3] = d->dma_phys + ((i + 1) % NIC_RX_COUNT) * 16; r->word[0] = NIC_OWN;
    }
    for (unsigned i = 0; i < NIC_TX_COUNT; i++) {
        struct nic_desc *t = nic_desc_at(d, true, i);
        t->word[1] = TU_CHAIN; t->word[2] = nic_buffer_phys(d, true, i);
        t->word[3] = d->dma_phys + NIC_TX_DESC + ((i + 1) % NIC_TX_COUNT) * 16;
    }
    uint8_t *setup = d->dma + 768;
    for (unsigned entry = 0; entry < 16; entry++) for (unsigned i = 0; i < 3; i++) {
        const uint8_t *mac = d->address;
        unsigned lo = entry == 15 ? 0xff : mac[i * 2], hi = entry == 15 ? 0xff : mac[i * 2 + 1];
        setup[entry * 12 + i * 4] = setup[entry * 12 + i * 4 + 2] = lo;
        setup[entry * 12 + i * 4 + 1] = setup[entry * 12 + i * 4 + 3] = hi;
    }
    struct nic_desc *s = (void *)(d->dma + 512);
    s->word[1] = (1u << 27) | (1u << 25) | 192; s->word[2] = d->dma_phys + 768; s->word[0] = NIC_OWN;
    net_dma_barrier(); net_write(d, 0x18, d->dma_phys, 4); net_write(d, 0x20, d->dma_phys + 512, 4);
    net_write(d, 0x30, d->media_control, 4); return 0;
}
static int tu_start(struct netif *d)
{
    struct nic_desc *s = (void *)(d->dma + 512);
    net_write(d, 0x30, d->media_control | 0x2000, 4); net_write(d, 8, 1, 4);
    for (unsigned i = 0; s->word[0] & NIC_OWN; i++) {
        if (i == 100) return -110;
        d->io.sleep(d->io.cookie, 1); net_dma_barrier();
    }
    /* Setup descriptor completion fields are undefined except ownership. */
    net_write(d, 0x30, d->media_control, 4);
    int err = tu_wait_idle(d); if (err) return err;
    net_write(d, 0x20, d->dma_phys + NIC_TX_DESC, 4); net_write(d, 0x28, TU_IRQS, 4);
    net_dma_barrier(); net_write(d, 0x30, d->media_control | TU_RUN, 4);
    return (net_read(d, 0x30, 4) & TU_RUN) == TU_RUN ? 0 : -5;
}
static int tu_stop(struct netif *d)
{
    tu_irq_enable(d, false); net_write(d, 0x30, d->media_control, 4);
    int err = tu_wait_idle(d);
    if (!err) { net_write(d, 0x18, 0, 4); net_write(d, 0x20, 0, 4); }
    return err;
}
static bool tu_descriptor(struct netif *d, bool tx, unsigned i)
{
    struct nic_desc *r = nic_desc_at(d, tx, i);
    return r->word[2] == nic_buffer_phys(d, tx, i) && r->word[3] ==
        d->dma_phys + (tx ? NIC_TX_DESC : NIC_RX_DESC) + ((i + 1) % (tx ? NIC_TX_COUNT : NIC_RX_COUNT)) * 16;
}
static int tu_transmit(struct netif *d, const uint8_t *data, unsigned len)
{
    unsigned i = d->tx_head; if (d->tx_used[i]) return -16;
    struct nic_desc *t = nic_desc_at(d, true, i);
    if (!tu_descriptor(d, true, i) || (t->word[0] & NIC_OWN)) return -5;
    len = nic_tx_copy(d, i, data, len); t->word[1] = TU_CHAIN | 0xe0000000u | len;
    net_dma_barrier(); t->word[0] = NIC_OWN; net_dma_barrier(); net_write(d, 8, 1, 4);
    d->tx_head = (i + 1) % NIC_TX_COUNT; return 0;
}
static int tu_poll(struct netif *d, uint8_t *out)
{
    uint32_t s = net_read(d, 0x28, 4), cause = s | net_take_pending(d);
    net_write(d, 0x28, s & TU_IRQS, 4);
    if (s == 0xffffffff || (cause & (0x2000 | 0x20 | 8))) return -5;
    net_dma_barrier();
    for (unsigned i = 0; i < NIC_TX_COUNT; i++) if (d->tx_used[i]) {
        struct nic_desc *t = nic_desc_at(d, true, i); if (!tu_descriptor(d, true, i)) return -5;
        if (!(t->word[0] & NIC_OWN)) net_tx_complete(d, i, !(t->word[0] & 0x8000));
        else if (net_expired(d, d->tx_deadline[i])) { d->stats.tx_errors++; return -110; }
    }
    struct nic_desc *r = nic_desc_at(d, false, d->rx_head); s = r->word[0];
    if (s & NIC_OWN) return 0;
    unsigned bytes = (s >> 16) & 0x3fff;
    if (!tu_descriptor(d, false, d->rx_head) || r->word[1] != (TU_CHAIN | 1536) || bytes < 18 || bytes > NET_FRAME_MAX + 4) {
        d->stats.rx_errors++; return -5;
    }
    bool good = (s & 0x8300) == 0x300;
    if (good) memcpy(out, nic_buffer(d, false, d->rx_head), bytes - 4); else d->stats.rx_errors++;
    net_dma_barrier(); r->word[0] = NIC_OWN; net_dma_barrier(); net_write(d, 0x10, 1, 4);
    d->rx_head = (d->rx_head + 1) % NIC_RX_COUNT; return good ? (int)bytes - 4 : 0;
}
static enum net_link tu_link(struct netif *d)
{
    if (d->phy != 0xff) return nic_mii_link(d, &tu_mii);
    uint32_t s = net_read(d, 0x60, 4);
    if (s == 0xffffffff) { d->fault_pending = true; return NET_LINK_UNKNOWN; }
    return s & 4 ? NET_LINK_DOWN : NET_LINK_UP;
}
const struct netif_driver net_tulip = {
    "tulip", tu_match, 128, 0, NIC_DMA_PAGES, tu_idle, tu_init, tu_start, tu_stop,
    tu_transmit, tu_poll, tu_link, nic_mac, tu_interrupt, tu_irq_enable, 0
};
