/* Intel 82557/8/9 and PRO/100 VE: simplified CU blocks and eight RFDs.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/net.h>

#define E100_RX 8u
#define E100_SETUP 16384u
#define E100_TX 20480u
struct e100_block {
    volatile uint16_t status, command;
    uint32_t link, buffer;
    volatile uint16_t count;
    uint16_t size;
    uint8_t data[NET_FRAME_MAX];
};
_Static_assert(offsetof(struct e100_block, data) == 16, "e100 descriptor layout");
static struct e100_block *e100_rfd(struct netif *d, unsigned slot)
{ return (struct e100_block *)(void *)(d->dma + slot * 2048); }
static bool e100_match(const struct pci_func *p)
{
    if (p->vendor != 0x8086 || p->class_code != 2 || p->subclass) return false;
    return p->device == 0x1229 || p->device == 0x1209 || p->device == 0x1030 ||
           p->device == 0x1031 || p->device == 0x1032 || p->device == 0x1033 ||
           p->device == 0x1034 || p->device == 0x1038 || p->device == 0x1039 ||
           p->device == 0x103a || p->device == 0x103b;
}
static bool e100_idle(struct netif *d)
{
    unsigned status = net_read(d, 0, 2);
    return status != 0xffff && !(status & 0xfc) && !net_read(d, 2, 1);
}
static void e100_irq_enable(struct netif *d, bool enable) { net_write(d, 3, enable ? 0 : 1, 1); }
static bool e100_interrupt(struct netif *d)
{
    unsigned causes = net_read(d, 1, 1) & 0xfc;
    if (!causes) return false;
    e100_irq_enable(d, false); d->pending |= causes; net_write(d, 1, causes, 1); return true;
}
static int e100_command(struct netif *d, unsigned command, uint32_t pointer)
{
    uint64_t deadline = d->io.now(d->io.cookie) + 100;
    for (unsigned n = 0; net_read(d, 2, 1); n++) {
        if (n >= 100 || net_expired(d, deadline)) return -110;
        d->io.sleep(d->io.cookie, 1);
    }
    net_dma_barrier(); net_write(d, 4, pointer, 4); net_write(d, 2, command, 1);
    return 0;
}
/* EEPROM READ only. Dummy zero determines six/eight address bits. Clock
 * high/low are 2 us each, CS deasserted >= 1 us; all in thread context. */
static int e100_ee_clock(struct netif *d, unsigned bit, unsigned *sample)
{
    unsigned pins = 2 | (bit ? 4 : 0);
    net_write(d, 0x0e, pins, 2);
    int err = d->io.delay(d->io.cookie, 2); if (err) return err;
    net_write(d, 0x0e, pins | 1, 2);
    err = d->io.delay(d->io.cookie, 2); if (err) return err;
    net_write(d, 0x0e, pins, 2); *sample = (net_read(d, 0x0e, 2) >> 3) & 1;
    return 0;
}
static int e100_ee_word(struct netif *d, unsigned address, unsigned *value, bool detect)
{
    unsigned bit, word = 0; int err = 0;
    net_write(d, 0x0e, 0, 2); err = d->io.delay(d->io.cookie, 2);
    for (unsigned i = 0; !err && i < 3; i++) err = e100_ee_clock(d, (6u >> (2 - i)) & 1, &bit);
    unsigned count = detect ? 8 : d->eeprom_bits;
    for (unsigned i = 0; !err && i < count; i++) {
        err = e100_ee_clock(d, detect ? 0 : ((address >> (count - i - 1)) & 1), &bit);
        if (detect && !err && !bit) { d->eeprom_bits = i + 1; break; }
    }
    if (!err && detect && d->eeprom_bits != 6 && d->eeprom_bits != 8) err = -5;
    for (unsigned i = 0; !err && i < 16; i++) {
        err = e100_ee_clock(d, 0, &bit); word = (word << 1) | bit;
    }
    net_write(d, 0x0e, 0, 2); if (!err) err = d->io.delay(d->io.cookie, 2);
    *value = word; return err;
}
static int e100_mii(struct netif *d, unsigned phy, unsigned reg, uint16_t *value)
{
    net_write(d, 0x10, (2u << 26) | (phy << 21) | (reg << 16), 4);
    uint64_t deadline = d->io.now(d->io.cookie) + 20;
    for (unsigned i = 0; i < 20; i++) {
        uint32_t mdi = net_read(d, 0x10, 4);
        if (mdi & (1u << 28)) { *value = mdi; return 0; }
        if (net_expired(d, deadline)) break;
        d->io.sleep(d->io.cookie, 1);
    }
    return -110;
}
static int e100_init(struct netif *d)
{
    e100_irq_enable(d, false);
    net_write(d, 8, 0, 4); d->io.sleep(d->io.cookie, 1);
    if (!e100_idle(d)) return -5;
    e100_irq_enable(d, false); net_write(d, 1, 0xfc, 1);
    unsigned word;
    int err = e100_ee_word(d, 0, &word, true);
    for (unsigned i = 0; !err && i < 3; i++) {
        err = e100_ee_word(d, i, &word, false);
        d->address[i * 2] = word; d->address[i * 2 + 1] = word >> 8;
    }
    if (err) return err;
    d->phy = 0xff;
    for (unsigned i = 0; i < 32; i++) {
        uint16_t status;
        err = e100_mii(d, i, 1, &status); if (err) return err;
        if (status && status != 0xffff) { d->phy = i; break; }
    }
    if (d->phy == 0xff) return -19;
    d->rx_head = 0; d->rx_tail = E100_RX - 1;
    for (unsigned i = 0; i < E100_RX; i++) {
        struct e100_block *rfd = e100_rfd(d, i);
        rfd->command = i == d->rx_tail ? 0x8000 : 0;
        rfd->link = d->dma_phys + ((i + 1) % E100_RX) * 2048;
        rfd->buffer = 0xffffffff; rfd->size = NET_FRAME_MAX;
    }
    return 0;
}
static int e100_action(struct netif *d, unsigned code, const uint8_t *data, unsigned length)
{
    volatile uint16_t *status = (volatile uint16_t *)(void *)(d->dma + E100_SETUP);
    memset(d->dma + E100_SETUP, 0, 64);
    status[1] = 0x8000 | code; *(uint32_t *)(void *)(d->dma + E100_SETUP + 4) = 0xffffffff;
    memcpy(d->dma + E100_SETUP + 8, data, length);
    int err = e100_command(d, 0x10, d->dma_phys + E100_SETUP); if (err) return err;
    uint64_t deadline = d->io.now(d->io.cookie) + 100;
    for (unsigned i = 0; i < 100; i++) {
        net_dma_barrier();
        if ((*status & 0x8000) && !(net_read(d, 0, 2) & 0xc0)) return (*status & 0x2000) ? 0 : -5;
        if (net_expired(d, deadline)) break;
        d->io.sleep(d->io.cookie, 1);
    }
    return -110;
}
static int e100_start(struct netif *d)
{
    /* Sections 6.4.2.3 tables 37–39: base settings shared by 82557/8/9,
     * standard (not extended) TCB/RFD, MII, discard short/bad frames,
     * broadcast, CRC strip, auto-duplex. Values are hardware fields. */
    uint8_t config[22] = {22,8,0,0,0,0,0x32,3,1,0,0x2e,0,0x60,0,0xf2,0x48,0,0x40,0xf2,0x80,0x3f,5};
    /* Byte 12 bit 0 is mandatory 1 on 82558 and later (tables 38–39).
     * 82557 shares device 1229 with later chips; revisions 0–3 are 82557. */
    if (d->pci.device != 0x1229 || d->pci.revision >= 4) config[12] |= 1;
    int err = e100_command(d, 0x60, 0);
    if (!err) err = e100_command(d, 6, 0);
    if (!err) err = e100_action(d, 2, config, sizeof(config));
    if (!err) err = e100_action(d, 1, d->address, 6);
    if (!err) err = e100_command(d, 1, d->dma_phys);
    return err;
}
static int e100_stop(struct netif *d)
{
    e100_irq_enable(d, false);
    /* Selective reset explicitly stops CU/RU, retaining configuration. */
    net_write(d, 8, 2, 4); d->io.sleep(d->io.cookie, 1);
    uint64_t deadline = d->io.now(d->io.cookie) + 100;
    for (unsigned i = 0; i < 100; i++) {
        if (e100_idle(d)) { e100_irq_enable(d, false); net_write(d, 1, 0xfc, 1); return 0; }
        if (net_expired(d, deadline)) break;
        d->io.sleep(d->io.cookie, 1);
    }
    return -110;
}
static int e100_transmit(struct netif *d, const uint8_t *data, unsigned length)
{
    if (d->tx_used[0] || net_read(d, 2, 1) || (net_read(d, 0, 2) & 0xc0)) return -16;
    struct e100_block *cb = (struct e100_block *)(void *)(d->dma + E100_TX);
    memset(cb, 0, sizeof(*cb)); cb->command = 0xa004; cb->link = cb->buffer = 0xffffffff;
    memcpy(cb->data, data, length);
    if (length < 60) length = 60;
    cb->count = 0x8000 | length;
    /* TCB bytes 14/15 are threshold and TBD count (not RFD size). */
    cb->size = 8;
    d->tx_used[0] = true; d->tx_length[0] = length;
    d->tx_deadline[0] = d->io.now(d->io.cookie) + NET_TX_TIMEOUT_MS;
    return e100_command(d, 0x10, d->dma_phys + E100_TX);
}
static int e100_poll(struct netif *d, uint8_t *out)
{
    unsigned status = net_read(d, 0, 2);
    if (status == 0xffff) return -5;
    net_write(d, 1, (status >> 8) & 0xfc, 1); (void)net_take_pending(d);
    if (d->tx_used[0]) {
        struct e100_block *cb = (struct e100_block *)(void *)(d->dma + E100_TX);
        net_dma_barrier();
        /* C is DMA completion, not on-wire delivery. Also wait CU idle
         * before reusing its block. ARP receipt proves actual delivery. */
        if ((cb->status & 0x8000) && !(status & 0xc0)) net_tx_complete(d, 0, (cb->status & 0x2000) && !(cb->status & 0x1000));
        else if (net_expired(d, d->tx_deadline[0])) { d->stats.tx_errors++; return -110; }
    }
    struct e100_block *rfd = e100_rfd(d, d->rx_head);
    net_dma_barrier();
    int length = 0;
    if (rfd->status & 0x8000) {
        unsigned count = rfd->count & 0x3fff;
        if (count > NET_FRAME_MAX || !(rfd->count & 0x4000) || !(rfd->count & 0x8000)) { d->stats.rx_errors++; return -5; }
        if ((rfd->status & 0x2000) && !(rfd->status & 0x0fff) && count >= 60) {
            memcpy(out, rfd->data, count); length = count;
        } else d->stats.rx_errors++;
        /* Publish a new end marker before clearing the old one. Completed
         * RFDs remain CPU-owned until all fields have been reinitialized. */
        rfd->command = 0x8000; rfd->count = 0; rfd->status = 0;
        net_dma_barrier(); e100_rfd(d, d->rx_tail)->command = 0;
        net_dma_barrier(); d->rx_tail = d->rx_head; d->rx_head = (d->rx_head + 1) % E100_RX;
    }
    unsigned ru = (net_read(d, 0, 2) >> 2) & 15;
    if (ru != 4 && !(e100_rfd(d, d->rx_head)->status & 0x8000)) {
        /* End-of-list / no resources: restart only on an empty descriptor,
         * not on a completed frame awaiting thread consumption. */
        if (net_read(d, 2, 1)) return length;
        int err = e100_command(d, 1, d->dma_phys + d->rx_head * 2048);
        if (err) return err;
    }
    return length;
}
static enum net_link e100_link(struct netif *d)
{
    uint16_t status;
    if (e100_mii(d, d->phy, 1, &status) || e100_mii(d, d->phy, 1, &status) || status == 0xffff) {
        d->fault_pending = true; return NET_LINK_UNKNOWN;
    }
    return (status & 4) ? NET_LINK_UP : NET_LINK_DOWN;
}
static void e100_mac(struct netif *d, uint8_t mac[6]) { memcpy(mac, d->address, 6); }
const struct netif_driver net_e100 = {
    "e100", e100_match, 32, 1, 6, e100_idle, e100_init, e100_start, e100_stop,
    e100_transmit, e100_poll, e100_link, e100_mac, e100_interrupt, e100_irq_enable
};
