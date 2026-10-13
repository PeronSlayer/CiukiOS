/* Private bounded storage for the F6-08 controllers. Register formats stay
 * in each driver. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NET_DMA_H
#define CIUKI_NET_DMA_H
#include <ciuki/kernel.h>
#include <ciuki/net.h>
#define NIC_RX_COUNT 8u
#define NIC_TX_COUNT 4u
#define NIC_RX_DESC 0u
#define NIC_TX_DESC 256u
#define NIC_RX_DATA 4096u
#define NIC_TX_DATA 20480u
#define NIC_BUFFER 2048u
#define NIC_DMA_PAGES 7u
#define NIC_OWN 0x80000000u
struct nic_desc { volatile uint32_t word[4]; };
_Static_assert(sizeof(struct nic_desc) == 16, "NIC descriptor size");
static inline struct nic_desc *nic_desc_at(struct netif *d, bool tx, unsigned slot)
{ return (struct nic_desc *)(d->dma + (tx ? NIC_TX_DESC : NIC_RX_DESC) + slot * 16); }
static inline uint32_t nic_buffer_phys(struct netif *d, bool tx, unsigned slot)
{ return d->dma_phys + (tx ? NIC_TX_DATA : NIC_RX_DATA) + slot * NIC_BUFFER; }
static inline uint8_t *nic_buffer(struct netif *d, bool tx, unsigned slot)
{ return d->dma + (tx ? NIC_TX_DATA : NIC_RX_DATA) + slot * NIC_BUFFER; }
static inline unsigned nic_tx_copy(struct netif *d, unsigned slot, const uint8_t *data, unsigned len)
{
    memcpy(nic_buffer(d, true, slot), data, len);
    if (len < 60) { memset(nic_buffer(d, true, slot) + len, 0, 60 - len); len = 60; }
    d->tx_used[slot] = true; d->tx_length[slot] = len;
    d->tx_deadline[slot] = d->io.now(d->io.cookie) + NET_TX_TIMEOUT_MS;
    return len;
}
static inline void nic_mac(struct netif *d, uint8_t mac[6]) { memcpy(mac, d->address, 6); }
/* Serial MII management, used only in thread context. A clock phase is one
 * microsecond, with an explicit output/input encoding for each controller. */
struct nic_mii { unsigned reg, width; uint32_t clock, data, sample, output, input; };
static inline int nic_mii_bit(struct netif *d, const struct nic_mii *m, bool read, unsigned bit, unsigned *sample)
{
    uint32_t v = (read ? m->input : m->output) | (bit ? m->data : 0);
    net_write(d, m->reg, v, m->width);
    if (d->io.delay(d->io.cookie, 1)) return -110;
    /* PHY data is stable during the low phase preceding the sampling edge.
     * Reading here also avoids QEMU Tulip's rising-edge shift of MDI. */
    if (sample) *sample = !!(net_read(d, m->reg, m->width) & m->sample);
    net_write(d, m->reg, v | m->clock, m->width);
    if (d->io.delay(d->io.cookie, 1)) return -110;
    return 0;
}
static inline int nic_mii_read(struct netif *d, const struct nic_mii *m, unsigned phy, unsigned reg, uint16_t *out)
{
    for (unsigned i = 0; i < 32; i++) if (nic_mii_bit(d, m, false, 1, 0)) return -110;
    unsigned command = (6u << 10) | (phy << 5) | reg;
    for (int i = 13; i >= 0; i--) if (nic_mii_bit(d, m, false, (command >> i) & 1, 0)) return -110;
    unsigned bit, value = 0;
    if (nic_mii_bit(d, m, true, 0, 0) || nic_mii_bit(d, m, true, 0, &bit)) return -110;
    bool absent = bit;
    for (unsigned i = 0; i < 16; i++) {
        if (nic_mii_bit(d, m, true, 0, &bit)) return -110;
        value = (value << 1) | bit;
    }
    net_write(d, m->reg, m->input, m->width); *out = absent ? 0xffff : value;
    return 0;
}
static inline int nic_mii_find(struct netif *d, const struct nic_mii *m)
{
    for (unsigned i = 0; i < 32; i++) {
        uint16_t status;
        if (nic_mii_read(d, m, i, 1, &status)) return -110;
        if (status && status != 0xffff) { d->phy = i; return 0; }
    }
    d->phy = 0xff; return -19;
}
static inline int nic_mii_write(struct netif *d, const struct nic_mii *m, unsigned phy, unsigned reg, uint16_t value)
{
    for (unsigned i = 0; i < 32; i++) if (nic_mii_bit(d, m, false, 1, 0)) return -110;
    uint32_t command = (5u << 28) | (phy << 23) | (reg << 18) | (2u << 16) | value;
    for (int i = 31; i >= 0; i--) if (nic_mii_bit(d, m, false, (command >> i) & 1, 0)) return -110;
    net_write(d, m->reg, m->input, m->width); return 0;
}
/* The initial 10/100 path advertises half duplex only. This avoids claiming
 * MAC/PHY duplex synchronization before hardware media qualification. */
static inline int nic_mii_half(struct netif *d, const struct nic_mii *m)
{
    return nic_mii_write(d, m, d->phy, 4, 0x00a1) ||
           nic_mii_write(d, m, d->phy, 0, 0x1200) ? -110 : 0;
}
static inline enum net_link nic_mii_link(struct netif *d, const struct nic_mii *m)
{
    uint16_t status;
    if (nic_mii_read(d, m, d->phy, 1, &status) || nic_mii_read(d, m, d->phy, 1, &status) || status == 0xffff) {
        d->fault_pending = true; return NET_LINK_UNKNOWN;
    }
    return status & 4 ? NET_LINK_UP : NET_LINK_DOWN;
}
#endif
