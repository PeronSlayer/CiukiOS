/* RTL8029 / NE2000 PCI, DP8390 page registers and word-wide remote DMA.
 * All packet memory is on the card: no PCI bus-master DMA.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/net.h>

#define NE_TX_PAGE 0x40u
#define NE_FIRST 0x46u
#define NE_STOP 0x80u
#define NE_IRQS 0x1fu
static bool ne_match(const struct pci_func *p)
{ return p->vendor == 0x10ec && p->device == 0x8029 && p->class_code == 2 && p->subclass == 0; }
static bool ne_idle(struct netif *d)
{
    unsigned cr = net_read(d, 0, 1);
    return cr != 0xff && (cr & 1) && !(cr & 4) && !(cr & 0xc0);
}
static unsigned ne_control(struct netif *d) { return d->running ? 0x22 : 0x21; }
static void ne_irq_enable(struct netif *d, bool enable) { net_write(d, 0x0f, enable ? NE_IRQS : 0, 1); }
static bool ne_interrupt(struct netif *d)
{
    /* Thread page accesses exclude IRQs only for the register pair. Remote
     * DMA runs in page 0 with IF=1, so an ack cannot change its DMA cursor. */
    unsigned status = net_read(d, 7, 1) & NE_IRQS;
    if (!status) return false;
    ne_irq_enable(d, false); d->pending |= status; net_write(d, 7, status, 1); return true;
}
static int ne_dma(struct netif *d, unsigned address, uint8_t *data, unsigned length, bool write)
{
    unsigned count = (length + 1) & ~1u;
    net_write(d, 0, ne_control(d), 1); net_write(d, 7, 0x40, 1);
    net_write(d, 8, address & 0xff, 1); net_write(d, 9, address >> 8, 1);
    net_write(d, 0x0a, count & 0xff, 1); net_write(d, 0x0b, count >> 8, 1);
    net_write(d, 0, (d->running ? 2u : 1u) | (write ? 0x10u : 8u), 1);
    for (unsigned i = 0; i < count; i += 2) {
        if (write) net_write(d, 0x10, data[i] | (unsigned)(i + 1 < length ? data[i + 1] : 0) << 8, 2);
        else {
            unsigned v = net_read(d, 0x10, 2);
            data[i] = v; if (i + 1 < length) data[i + 1] = v >> 8;
        }
        if ((i & 127) == 126) task_yield(); /* never a long PIO/IF=0 loop */
    }
    uint64_t deadline = d->io.now(d->io.cookie) + 20;
    for (unsigned n = 0; n < 20; n++) {
        unsigned isr = net_read(d, 7, 1);
        if (isr == 0xff) return -5;
        if (isr & 0x40) {
            net_write(d, 7, 0x40, 1); net_write(d, 0, ne_control(d), 1); return 0;
        }
        if (net_expired(d, deadline)) break;
        d->io.sleep(d->io.cookie, 1);
    }
    return -110;
}
static int ne_reset(struct netif *d)
{
    unsigned latch = net_read(d, 0x1f, 1); net_write(d, 0x1f, latch, 1);
    uint64_t deadline = d->io.now(d->io.cookie) + 100;
    for (unsigned i = 0; i < 100; i++) {
        unsigned isr = net_read(d, 7, 1);
        if (isr == 0xff) return -5;
        if ((isr & 0x80) && ne_idle(d)) return 0;
        if (net_expired(d, deadline)) break;
        d->io.sleep(d->io.cookie, 1);
    }
    return -110;
}
static int ne_init(struct netif *d)
{
    ne_irq_enable(d, false);
    int err = ne_reset(d); if (err) return err;
    net_write(d, 0, 0x21, 1); ne_irq_enable(d, false);
    net_write(d, 0x0e, 0x49, 1); /* word, normal byte order, FIFO=8 bytes */
    net_write(d, 0x0a, 0, 1); net_write(d, 0x0b, 0, 1);
    net_write(d, 0x0c, 0x20, 1); net_write(d, 0x0d, 2, 1);
    uint8_t prom[32]; err = ne_dma(d, 0, prom, sizeof(prom), false);
    if (err) return err;
    /* RTL8029AS section 6.2.2 describes packed PROM bytes. Conventional
     * NE2000 word PROM (also QEMU) duplicates each byte. Use the documented
     * W signature to distinguish layouts rather than guessing a valid MAC. */
    unsigned stride;
    if (prom[28] == 0x57 && prom[29] == 0x57 && prom[30] == 0x57 && prom[31] == 0x57) {
        stride = 2;
        for (unsigned i = 0; i < 6; i++) if (prom[i * 2] != prom[i * 2 + 1]) return -5;
    } else if (prom[14] == 0x57 && prom[15] == 0x57) stride = 1;
    else return -5;
    for (unsigned i = 0; i < 6; i++) d->address[i] = prom[i * stride];
    net_write(d, 1, NE_FIRST, 1); net_write(d, 2, NE_STOP, 1);
    net_write(d, 3, NE_FIRST, 1); net_write(d, 4, NE_TX_PAGE, 1);
    uint32_t flags = irq_save();
    net_write(d, 0, 0x61, 1);
    for (unsigned i = 0; i < 6; i++) net_write(d, 1 + i, d->address[i], 1);
    net_write(d, 7, NE_FIRST + 1, 1);
    net_write(d, 0, 0x21, 1); irq_restore(flags);
    d->next_page = NE_FIRST + 1; net_write(d, 7, 0x7f, 1);
    return 0;
}
static int ne_start(struct netif *d)
{
    net_write(d, 0, 0x22, 1); net_write(d, 0x0d, 0, 1);
    net_write(d, 0x0c, 4, 1); /* broadcast plus physical address, no runt/errors */
    unsigned cr = net_read(d, 0, 1);
    return cr != 0xff && (cr & 3) == 2 ? 0 : -5;
}
static int ne_stop(struct netif *d)
{
    ne_irq_enable(d, false); net_write(d, 0, 0x21, 1);
    int err = ne_reset(d);
    if (!err) { net_write(d, 0x0a, 0, 1); net_write(d, 0x0b, 0, 1); }
    return err;
}
static int ne_transmit(struct netif *d, const uint8_t *data, unsigned length)
{
    if (d->tx_used[0]) return -16;
    /* Per-device scratch is protected by the device mutex, never on the
     * IRQ stack. Length is bounded by the framework before entry. */
    memcpy(d->scratch.data, data, length);
    if (length < 60) { memset(d->scratch.data + length, 0, 60 - length); length = 60; }
    int err = ne_dma(d, NE_TX_PAGE << 8, d->scratch.data, length, true); if (err) return err;
    net_write(d, 7, 0x0a, 1); net_write(d, 4, NE_TX_PAGE, 1);
    net_write(d, 5, length & 0xff, 1); net_write(d, 6, length >> 8, 1);
    d->tx_used[0] = true; d->tx_length[0] = length;
    d->tx_deadline[0] = d->io.now(d->io.cookie) + NET_TX_TIMEOUT_MS;
    net_write(d, 0, 0x26, 1); return 0;
}
static unsigned ne_current(struct netif *d)
{
    uint32_t flags = irq_save(); net_write(d, 0, 0x62, 1);
    unsigned page = net_read(d, 7, 1); net_write(d, 0, 0x22, 1); irq_restore(flags); return page;
}
static int ne_ring_read(struct netif *d, unsigned addr, uint8_t *out, unsigned count)
{
    unsigned first = (NE_STOP << 8) - addr; if (first > count) first = count;
    int err = ne_dma(d, addr, out, first, false);
    if (!err && first < count) err = ne_dma(d, NE_FIRST << 8, out + first, count - first, false);
    return err;
}
static int ne_poll(struct netif *d, uint8_t *out)
{
    unsigned status = net_read(d, 7, 1) | net_take_pending(d);
    net_write(d, 7, status & 0x3f, 1);
    if (status == 0xff || (status & 0x10)) { d->stats.rx_errors++; return -5; }
    if (status & 4) d->stats.rx_errors++;
    if (status & 0x20) { /* drain the three tally counters (read clears) */
        (void)net_read(d, 0x0d, 1); (void)net_read(d, 0x0e, 1); (void)net_read(d, 0x0f, 1);
    }
    if (d->tx_used[0]) {
        if (status & 0x0a) {
            unsigned tsr = net_read(d, 4, 1);
            net_tx_complete(d, 0, (status & 2) && (tsr & 1) && !(tsr & 0x38));
        } else if (net_expired(d, d->tx_deadline[0])) { d->stats.tx_errors++; return -110; }
    }
    unsigned current = ne_current(d);
    if (current < NE_FIRST || current >= NE_STOP || d->next_page < NE_FIRST || d->next_page >= NE_STOP) return -5;
    if (current == d->next_page) return 0;
    uint8_t header[4]; int err = ne_dma(d, d->next_page << 8, header, 4, false); if (err) return err;
    unsigned next = header[1], count = header[2] | (unsigned)header[3] << 8;
    unsigned pages = (count + 4 + 255) / 256;
    unsigned expected = NE_FIRST + (d->next_page - NE_FIRST + pages) % (NE_STOP - NE_FIRST);
    unsigned available = (current + NE_STOP - NE_FIRST - d->next_page) % (NE_STOP - NE_FIRST);
    if (count < 64 || count > NET_FRAME_MAX + 4 || next != expected || pages > available) { d->stats.rx_errors++; return -5; }
    bool good = (header[0] & 1) && !(header[0] & 0x1e);
    if (good) { err = ne_ring_read(d, (d->next_page << 8) + 4, out, count - 4); if (err) return err; }
    else d->stats.rx_errors++;
    d->next_page = next; net_write(d, 3, next == NE_FIRST ? NE_STOP - 1 : next - 1, 1);
    return good ? (int)(count - 4) : 0;
}
static enum net_link ne_link(struct netif *d) { (void)d; return NET_LINK_UNKNOWN; }
static void ne_mac(struct netif *d, uint8_t mac[6]) { memcpy(mac, d->address, 6); }
const struct netif_driver net_ne2000 = {
    "ne2000", ne_match, 32, 0, 0, ne_idle, ne_init, ne_start, ne_stop,
    ne_transmit, ne_poll, ne_link, ne_mac, ne_interrupt, ne_irq_enable
};
