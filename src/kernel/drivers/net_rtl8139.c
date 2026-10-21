/* RTL8139 C-mode: 8 KiB RX ring and four private TX buffers.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/net.h>

#define RTL_RING 8192u
#define RTL_TX 12288u
#define RTL_IRQS 0x807fu
static bool rtl_match(const struct pci_func *p)
{ return p->vendor == 0x10ec && p->device == 0x8139 && p->class_code == 2 && p->subclass == 0; }
static bool rtl_idle(struct netif *d)
{ unsigned cr = net_read(d, 0x37, 1); return cr != 0xff && !(cr & 0x1c); }
static void rtl_irq_enable(struct netif *d, bool enable)
{ net_write(d, 0x3c, enable ? RTL_IRQS : 0, 2); }
static bool rtl_interrupt(struct netif *d)
{
    unsigned status = net_read(d, 0x3e, 2);
    if (!(status & RTL_IRQS)) return false;
    rtl_irq_enable(d, false);
    d->pending |= status & RTL_IRQS;
    net_write(d, 0x3e, status & RTL_IRQS, 2);
    return true;
}
static int rtl_reset(struct netif *d)
{
    net_write(d, 0x37, 0x10, 1);
    uint64_t deadline = d->io.now(d->io.cookie) + 100;
    for (unsigned n = 0; n < 100; n++) {
        unsigned cr = net_read(d, 0x37, 1);
        if (cr == 0xff) return -5;
        if (!(cr & 0x10)) return 0;
        if (net_expired(d, deadline)) break;
        d->io.sleep(d->io.cookie, 1);
    }
    return -110;
}
static int rtl_init(struct netif *d)
{
    rtl_irq_enable(d, false);
    int err = rtl_reset(d); if (err) return err;
    rtl_irq_enable(d, false); net_write(d, 0x3e, RTL_IRQS, 2);
    for (unsigned i = 0; i < 6; i++) d->address[i] = net_read(d, i, 1);
    return 0;
}
static int rtl_start(struct netif *d)
{
    net_write(d, 0x30, d->dma_phys, 4);
    for (unsigned i = 0; i < 4; i++) net_write(d, 0x20 + i * 4, d->dma_phys + RTL_TX + i * 2048, 4);
    d->rx_offset = 0; net_write(d, 0x38, 0xfff0, 2);
    net_dma_barrier(); net_write(d, 0x37, 0x0c, 1);
    /* FIFO=whole packet, 256-byte DMA bursts, normal circular wrapping,
     * physical address and broadcast only; no error/runt/promiscuous RX. */
    net_write(d, 0x44, (7u << 13) | (4u << 8) | 0x0a, 4);
    net_write(d, 0x40, (3u << 24) | (4u << 8), 4);
    return (net_read(d, 0x37, 1) & 0x0c) == 0x0c ? 0 : -5;
}
static int rtl_stop(struct netif *d)
{
    rtl_irq_enable(d, false); net_write(d, 0x37, 0, 1);
    /* Healthy teardown uses documented reset completion to stop both DMA
     * engines. Fault teardown never calls this or frees retained buffers. */
    int err = rtl_reset(d);
    if (!err && !rtl_idle(d)) err = -5;
    if (!err) {
        net_write(d, 0x30, 0, 4);
        for (unsigned i = 0; i < 4; i++) net_write(d, 0x20 + i * 4, 0, 4);
    }
    return err;
}
static int rtl_transmit(struct netif *d, const uint8_t *data, unsigned len)
{
    unsigned slot = 0;
    while (slot < 4 && d->tx_used[slot]) slot++;
    if (slot == 4) return -16;
    uint8_t *buffer = d->dma + RTL_TX + slot * 2048;
    memcpy(buffer, data, len);
    if (len < 60) { memset(buffer + len, 0, 60 - len); len = 60; }
    d->tx_length[slot] = len; d->tx_used[slot] = true;
    d->tx_deadline[slot] = d->io.now(d->io.cookie) + NET_TX_TIMEOUT_MS;
    net_dma_barrier(); net_write(d, 0x10 + slot * 4, len | (8u << 16), 4);
    return 0;
}
static void rtl_copy(struct netif *d, unsigned pos, void *out, unsigned count)
{
    pos %= RTL_RING;
    unsigned first = RTL_RING - pos; if (first > count) first = count;
    memcpy(out, d->dma + pos, first);
    if (first < count) memcpy((uint8_t *)out + first, d->dma, count - first);
}
static int rtl_poll(struct netif *d, uint8_t *out)
{
    unsigned status = net_read(d, 0x3e, 2) | net_take_pending(d);
    net_write(d, 0x3e, status & RTL_IRQS, 2);
    if (status == 0xffff || (status & (0x8000 | 0x50))) return -5;
    for (unsigned i = 0; i < 4; i++) if (d->tx_used[i]) {
        uint32_t s = net_read(d, 0x10 + i * 4, 4);
        if (s & (1u << 30)) { d->stats.tx_errors++; return -5; }
        if ((s & (1u << 13)) && (s & ((1u << 15) | (1u << 14))))
            net_tx_complete(d, i, (s & (1u << 15)) && !(s & (1u << 14)));
        else if (net_expired(d, d->tx_deadline[i])) { d->stats.tx_errors++; return -110; }
    }
    if (net_read(d, 0x37, 1) & 1) return 0;
    net_dma_barrier(); uint8_t header[4]; rtl_copy(d, d->rx_offset, header, 4);
    unsigned flags = header[0] | (unsigned)header[1] << 8;
    unsigned bytes = header[2] | (unsigned)header[3] << 8;
    if (bytes == 0xfff0) return 0; /* DMA still in progress */
    if (bytes < 64 || bytes > NET_FRAME_MAX + 4) { d->stats.rx_errors++; return -5; }
    unsigned len = bytes - 4; /* strip the FCS */
    bool good = (flags & 1) && !(flags & 0x3e);
    if (good) rtl_copy(d, d->rx_offset + 4, out, len);
    else d->stats.rx_errors++;
    d->rx_offset = ((d->rx_offset + 4 + bytes + 3) & ~3u) % RTL_RING;
    net_dma_barrier(); net_write(d, 0x38, (d->rx_offset - 16) & 0xffff, 2);
    return good ? (int)len : 0;
}
static enum net_link rtl_link(struct netif *d)
{
    unsigned msr = net_read(d, 0x58, 1);
    if (msr == 0xff) { d->fault_pending = true; return NET_LINK_UNKNOWN; }
    return (msr & 4) ? NET_LINK_DOWN : NET_LINK_UP;
}
static void rtl_mac(struct netif *d, uint8_t mac[6]) { memcpy(mac, d->address, 6); }
static int rtl_promiscuous(struct netif *d, bool on)
{
    uint32_t config = net_read(d, 0x44, 4);
    if (config == UINT32_MAX) return -5;
    config = on ? config | 1u : config & ~1u;
    net_write(d, 0x44, config, 4);
    return net_read(d, 0x44, 4) == config ? 0 : -5;
}
const struct netif_driver net_rtl8139 = {
    "rtl8139", rtl_match, 256, 0, 5, rtl_idle, rtl_init, rtl_start, rtl_stop,
    rtl_transmit, rtl_poll, rtl_link, rtl_mac, rtl_interrupt, rtl_irq_enable, rtl_promiscuous
};
