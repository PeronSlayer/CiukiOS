/* AMD PCnet PCI: word I/O, software style 2 (32-bit DMA).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/cpu.h>
#include "net_dma.h"
#define PC_IRQS 0x7f00u
struct pc_init { uint16_t mode, rings; uint8_t mac[6]; uint16_t reserved; uint32_t filter[2], rx, tx; };
_Static_assert(sizeof(struct pc_init) == 28, "PCnet initialization block");
static uint16_t pc_read(struct netif *d, bool bcr, unsigned reg)
{
    uint32_t f = irq_save(); net_write(d, 0x12, reg, 2);
    uint16_t v = net_read(d, bcr ? 0x16 : 0x10, 2); irq_restore(f); return v;
}
static void pc_write(struct netif *d, bool bcr, unsigned reg, unsigned v)
{
    uint32_t f = irq_save(); net_write(d, 0x12, reg, 2); net_write(d, bcr ? 0x16 : 0x10, v, 2); irq_restore(f);
}
static bool pc_match(const struct pci_func *p)
{ return p->vendor == 0x1022 && p->device == 0x2000 && p->class_code == 2 && !p->subclass; }
static bool pc_idle(struct netif *d)
{ uint16_t s = pc_read(d, false, 0); return s != 0xffff && (s & 4) && !(s & 0x33); }
static void pc_irq_enable(struct netif *d, bool on)
{ pc_write(d, false, 0, on ? 0x40 : 0); }
static bool pc_interrupt(struct netif *d)
{
    unsigned s = pc_read(d, false, 0);
    if (s == 0xffff) { pc_irq_enable(d, false); d->fault_pending = true; return true; }
    if (!(s & 0x80)) return false;
    pc_write(d, false, 0, s & PC_IRQS); d->pending |= s & PC_IRQS; return true;
}
static int pc_init_device(struct netif *d)
{
    (void)net_read(d, 0x14, 2); /* software reset returns to word I/O */
    if (!pc_idle(d)) return -5;
    pc_write(d, true, 20, 2);
    if ((pc_read(d, true, 20) & 0xff) != 2) return -5;
    for (unsigned i = 0; i < 6; i++) d->address[i] = net_read(d, i, 1);
    unsigned chip = ((uint32_t)pc_read(d, false, 89) << 16 | pc_read(d, false, 88)) >> 12;
    chip &= 0xffff;
    /* 970/970A, 971/972 and 973 share style 2. Reject unrelated revisions. */
    if (chip != 0x2420 && chip != 0x2430 && chip != 0x2621 && chip != 0x2623 && chip != 0x2624 && chip != 0x2625) return -19;
    d->media_control = chip;
    pc_write(d, true, 2, pc_read(d, true, 2) | 2); /* automatic TP/AUI selection */
    if (chip == 0x2420 || chip == 0x2430 || chip == 0x2621) pc_write(d, true, 4, 0x40); /* link only */
    else d->phy = (pc_read(d, true, 33) >> 5) & 31;
    struct pc_init *b = (void *)(d->dma + 512);
    b->rings = (3u << 4) | (2u << 12); memcpy(b->mac, d->address, 6);
    b->rx = d->dma_phys; b->tx = d->dma_phys + NIC_TX_DESC;
    for (unsigned i = 0; i < NIC_RX_COUNT; i++) {
        struct nic_desc *r = nic_desc_at(d, false, i); r->word[0] = nic_buffer_phys(d, false, i);
        r->word[1] = NIC_OWN | 0xf800u; /* -2048, required ones in high nibble */
    }
    for (unsigned i = 0; i < NIC_TX_COUNT; i++) nic_desc_at(d, true, i)->word[0] = nic_buffer_phys(d, true, i);
    pc_write(d, false, 1, (d->dma_phys + 512) & 0xffff); pc_write(d, false, 2, (d->dma_phys + 512) >> 16);
    /* Software pads TX and strips RX FCS. Disable descriptor auto polling;
     * keep both hardware pad/strip bits clear (also on the QEMU twin). */
    pc_write(d, false, 4, 0x1000); net_dma_barrier(); return 0;
}
static int pc_start(struct netif *d)
{
    pc_write(d, false, 0, 1);
    for (unsigned n = 0; n < 100; n++) {
        unsigned s = pc_read(d, false, 0);
        if (s == 0xffff || (s & 0x800)) return -5;
        if (s & 0x100) {
            pc_write(d, false, 0, 0x102); s = pc_read(d, false, 0);
            return (s & 0x32) == 0x32 ? 0 : -5;
        }
        d->io.sleep(d->io.cookie, 1);
    }
    return -110;
}
static int pc_stop(struct netif *d)
{
    pc_write(d, false, 0, 4);
    for (unsigned n = 0; n < 100; n++) {
        if (pc_idle(d)) { pc_write(d, false, 1, 0); pc_write(d, false, 2, 0); return 0; }
        d->io.sleep(d->io.cookie, 1);
    }
    return -110;
}
static int pc_transmit(struct netif *d, const uint8_t *data, unsigned len)
{
    unsigned i = d->tx_head;
    if (d->tx_used[i]) return -16;
    struct nic_desc *t = nic_desc_at(d, true, i);
    if ((t->word[1] & NIC_OWN) || t->word[0] != nic_buffer_phys(d, true, i)) return -5;
    len = nic_tx_copy(d, i, data, len); t->word[2] = t->word[3] = 0;
    net_dma_barrier(); t->word[1] = NIC_OWN | 0x03000000u | ((-len) & 0xffffu);
    net_dma_barrier(); pc_write(d, false, 0, 8 | (d->irq_attached ? 0x40 : 0));
    d->tx_head = (i + 1) % NIC_TX_COUNT; return 0;
}
static int pc_poll(struct netif *d, uint8_t *out)
{
    unsigned s = pc_read(d, false, 0), cause = s | net_take_pending(d);
    pc_write(d, false, 0, s & PC_IRQS);
    /* ERR is a summary including recoverable MISS/CERR. Memory errors and
     * babbling are controller faults; missed RX is a counted drop. */
    if (s == 0xffff || (cause & 0x4800) || (s & 0x30) != 0x30) return -5;
    if (cause & 0x1000) d->stats.rx_dropped++;
    net_dma_barrier();
    for (unsigned i = 0; i < NIC_TX_COUNT; i++) if (d->tx_used[i]) {
        struct nic_desc *t = nic_desc_at(d, true, i);
        if (t->word[0] != nic_buffer_phys(d, true, i)) return -5;
        if (!(t->word[1] & NIC_OWN)) {
            if (t->word[2] & 0x40000000u) return -5; /* buffer underflow stops engine */
            net_tx_complete(d, i, !(t->word[1] & 0x40000000u));
        } else if (net_expired(d, d->tx_deadline[i])) { d->stats.tx_errors++; return -110; }
    }
    struct nic_desc *r = nic_desc_at(d, false, d->rx_head);
    s = r->word[1]; if (s & NIC_OWN) return 0;
    unsigned bytes = r->word[2] & 0xfff;
    if (r->word[0] != nic_buffer_phys(d, false, d->rx_head) || (s & 0xffff) != 0xf800 || bytes < 18 || bytes > NET_FRAME_MAX + 4) {
        d->stats.rx_errors++; return -5;
    }
    bool good = (s & 0x43000000u) == 0x03000000u;
    if (good) memcpy(out, nic_buffer(d, false, d->rx_head), bytes - 4); else d->stats.rx_errors++;
    r->word[2] = r->word[3] = 0; net_dma_barrier(); r->word[1] = NIC_OWN | 0xf800;
    d->rx_head = (d->rx_head + 1) % NIC_RX_COUNT; return good ? (int)bytes - 4 : 0;
}
static uint16_t pc_mii(struct netif *d, unsigned reg)
{
    pc_write(d, true, 33, (d->phy << 5) | reg); return pc_read(d, true, 34);
}
static enum net_link pc_link(struct netif *d)
{
    unsigned s;
    bool legacy = d->media_control == 0x2420 || d->media_control == 0x2430 || d->media_control == 0x2621;
    if (legacy) s = pc_read(d, true, 4);
    else { (void)pc_mii(d, 1); s = pc_mii(d, 1); }
    if (s == 0xffff) { d->fault_pending = true; return NET_LINK_UNKNOWN; }
    if (legacy) return s & 0x8000 ? NET_LINK_UP : NET_LINK_DOWN;
    return s & 4 ? NET_LINK_UP : NET_LINK_DOWN;
}
const struct netif_driver net_pcnet = {
    "pcnet", pc_match, 32, 0, NIC_DMA_PAGES, pc_idle, pc_init_device, pc_start, pc_stop,
    pc_transmit, pc_poll, pc_link, nic_mac, pc_interrupt, pc_irq_enable
};
