/* Synthetic bus/register/DMA models for the production F6-08 drivers.
 * Included by net_test.c to reuse its registry, PMM and scheduler fixtures.
 * SPDX-License-Identifier: GPL-2.0-only */
enum second_kind { S_E1000, S_PCNET, S_TULIP, S_3COM, S_RHINE };
struct second_model {
    struct model fixture;
    enum second_kind kind;
    uint32_t intel[0x6000 / 4], index;
    uint16_t csr[128], bcr[128], phy_regs[32];
    uint8_t windows[8][16];
    unsigned window, tx_cursor, rx_cursor, rx_base, tx_base, nic_writes;
    unsigned ee_count, ee_command, ee_address, ee_pins, ee_sample, ee_bits;
    unsigned mii_count, mii_word, mii_pins, mii_sample, mii_reg, mii_reads;
    unsigned phy_address, tx_status, command, delay_calls;
    bool absent, reset_stuck, init_stuck, stop_stuck, mii_stuck, ee_stuck, phy_reset_stuck;
    bool tx_auto, tx_error, wire_done, dma_prepared, qemu_mii_edge;
};
static const uint8_t second_mac[6] = {0x52,0x54,0,0x12,0x34,0x56};
static void second_dma_ready(struct second_model *h)
{
    struct netif *d = h->fixture.device;
    CHECK(d->dma && page_count == NIC_DMA_PAGES);
    CHECK(registry_valid(d->claims[2], d->claim_generation[2]));
    for (unsigned i = 0; i < NIC_RX_COUNT; i++) {
        struct nic_desc *r = nic_desc_at(d, false, i);
        unsigned field = h->kind == S_E1000 || h->kind == S_PCNET ? 0 : 2;
        CHECK(r->word[field] == nic_buffer_phys(d, false, i));
        CHECK(nic_buffer_phys(d, false, i) + NIC_BUFFER <= d->dma_phys + NIC_DMA_PAGES * PAGE_SIZE);
    }
    if (h->kind == S_E1000) CHECK(h->intel[0x2800 / 4] == d->dma_phys && h->intel[0x3808 / 4] == 128);
    if (h->kind == S_PCNET) {
        struct pc_init *b = model_dma((uint32_t)h->csr[2] << 16 | h->csr[1], sizeof(*b));
        CHECK(b->rx == d->dma_phys && b->tx == d->dma_phys + NIC_TX_DESC && b->rings == 0x2030);
        CHECK(!memcmp(b->mac, second_mac, 6) && !b->filter[0] && !b->filter[1]);
    }
    if (h->kind == S_TULIP) {
        CHECK(model_get(h->fixture.reg + 0x18, 4) == d->dma_phys);
        struct nic_desc *s = model_dma(model_get(h->fixture.reg + 0x20, 4), 16);
        CHECK(s->word[0] == NIC_OWN && (s->word[1] & 0x7ff) == 192);
    }
    if (h->kind == S_3COM) CHECK(model_get(h->fixture.reg + 0x38, 4) == d->dma_phys);
    if (h->kind == S_RHINE) CHECK(model_get(h->fixture.reg + 0x18, 4) == d->dma_phys);
    h->dma_prepared = true;
}
static void second_ee_clock(struct second_model *h, unsigned value)
{
    if (!(value & 1)) { h->ee_count = h->ee_command = h->ee_address = 0; h->ee_sample = 1; }
    else if ((value & 2) && !(h->ee_pins & 2)) {
        unsigned bit = !!(value & 4);
        if (h->ee_count < 3) h->ee_command = (h->ee_command << 1) | bit;
        else if (h->ee_count < 3 + h->ee_bits) {
            CHECK(h->ee_command == 6); h->ee_address = (h->ee_address << 1) | bit;
            h->ee_sample = h->ee_count + 1 == 3 + h->ee_bits ? 0 : 1;
        } else {
            unsigned i = h->ee_count - 3 - h->ee_bits; CHECK(i < 16);
            uint16_t word = h->ee_address >= 10 && h->ee_address <= 12 ?
                second_mac[(h->ee_address - 10) * 2] | (uint16_t)second_mac[(h->ee_address - 10) * 2 + 1] << 8 : 0;
            h->ee_sample = (word >> (15 - i)) & 1;
        }
        h->ee_count++;
    }
    h->ee_pins = value;
}
static void second_mii_clock(struct second_model *h, unsigned value)
{
    bool tulip = h->kind == S_TULIP, rhine = h->kind == S_RHINE;
    unsigned clock = tulip ? 0x10000 : 1, data = tulip ? 0x20000 : rhine ? 4 : 2;
    bool input = tulip ? !!(value & 0x40000) : !(value & (rhine ? 8 : 4));
    if (!h->qemu_mii_edge && input && !(value & clock) && h->mii_count >= 14 && h->mii_count < 32) {
        unsigned phy = (h->mii_word >> 5) & 31, next = h->mii_count + 1;
        unsigned v = phy == h->phy_address ? h->phy_regs[h->mii_reg] : 0xffff;
        h->mii_sample = next <= 16 ? phy != h->phy_address : (v >> (32 - next)) & 1;
    }
    if ((value & clock) && !(h->mii_pins & clock)) {
        unsigned bit = !!(value & data);
        if (!h->mii_count && !input && bit) { h->mii_word = 0; }
        else {
            h->mii_count++;
            if (!input) h->mii_word = (h->mii_word << 1) | bit;
            if (h->mii_count == 14) {
                unsigned phy = (h->mii_word >> 5) & 31;
                h->mii_reg = h->mii_word & 31;
                h->mii_sample = phy == h->phy_address ? 0 : 1;
            }
            if (input && h->mii_count >= 17 && h->mii_count <= 32) {
                unsigned phy = (h->mii_word >> 5) & 31;
                unsigned v = phy == h->phy_address ? h->phy_regs[h->mii_reg] : 0xffff;
                h->mii_sample = h->qemu_mii_edge ?
                    (h->mii_count == 32 ? 0 : (v >> (31 - h->mii_count)) & 1) : (v >> (32 - h->mii_count)) & 1;
            }
            if (h->mii_count == 32) {
                if (input) h->mii_reads++;
                else if (((h->mii_word >> 23) & 31) == h->phy_address)
                    h->phy_regs[(h->mii_word >> 18) & 31] = h->mii_word;
                h->mii_count = 0;
            }
        }
    }
    h->mii_pins = value;
}
static void second_tx(struct second_model *h)
{
    struct netif *d = h->fixture.device; CHECK(h->dma_prepared && (model_get(h->fixture.pci + 4, 2) & 4));
    if (!h->tx_auto) return;
    if (h->kind == S_E1000) {
        struct e1_tx *t = (void *)nic_desc_at(d, true, h->tx_cursor);
        CHECK(t->low == nic_buffer_phys(d, true, 0) && !t->high && t->command == 0x0b);
        CHECK(t->length >= 60 && t->length <= NET_FRAME_MAX);
        h->fixture.packet_io++; t->status = h->tx_error ? 3 : 1;
        h->tx_cursor = (h->tx_cursor + 1) % 8; h->intel[0xc0 / 4] |= 1;
        return;
    }
    if (h->kind == S_3COM) {
        struct nic_desc *t = model_dma(model_get(h->fixture.reg + 0x24, 4), 16);
        CHECK(!t->word[0] && (t->word[1] & 0x8000) && t->word[2] == nic_buffer_phys(d, true, 0));
        unsigned len = t->word[1] & 0x1fff; CHECK(len >= 60 && len <= NET_FRAME_MAX && t->word[3] == (NIC_OWN | len));
        h->fixture.packet_io++; model_put(h->fixture.reg + 0x24, 0, 4);
        h->tx_status = h->wire_done ? (h->tx_error ? 0x88 : 0xc0) : 0;
        model_put(h->fixture.reg + 0x0e, model_get(h->fixture.reg + 0x0e, 2) | 0x204, 2); return;
    }
    for (unsigned n = 0; n < NIC_TX_COUNT; n++) {
        struct nic_desc *t = nic_desc_at(d, true, h->tx_cursor);
        unsigned own = h->kind == S_PCNET ? 1 : 0; if (!(t->word[own] & NIC_OWN)) break;
        unsigned field = h->kind == S_PCNET ? 0 : 2;
        CHECK(t->word[field] == nic_buffer_phys(d, true, h->tx_cursor));
        unsigned len = h->kind == S_PCNET ? (-t->word[1]) & 0xfff : t->word[1] & 0x7ff;
        CHECK(len >= 60 && len <= NET_FRAME_MAX); h->fixture.packet_io++;
        if (h->kind == S_PCNET) { CHECK((t->word[1] & 0x03000000) == 0x03000000); t->word[1] &= ~NIC_OWN; if (h->tx_error) t->word[1] |= 0x40000000; h->csr[0] |= 0x200; }
        else { t->word[0] = h->tx_error ? 0x8000 : 0; if (h->kind == S_TULIP) model_put(h->fixture.reg + 0x28, model_get(h->fixture.reg + 0x28, 4) | 1, 4); else model_put(h->fixture.reg + 0x0c, model_get(h->fixture.reg + 0x0c, 2) | 2, 2); }
        h->tx_cursor = (h->tx_cursor + 1) % NIC_TX_COUNT;
    }
}
static uint32_t second_read(void *arg, uint16_t port, unsigned width)
{
    struct second_model *h = arg;
    if (port == 0xcfc || port == 0x4d1) return model_read(&h->fixture, port, width);
    CHECK(port >= 0x1000 && port < 0x1100); unsigned r = port - 0x1000;
    cycles += h->fixture.read_cost;
    if (h->absent) return width == 1 ? 0xff : width == 2 ? 0xffff : 0xffffffff;
    if (h->kind == S_E1000) {
        CHECK(width == 4 && (r == 0 || r == 4));
        if (!r) return h->index;
        CHECK(h->index < sizeof(h->intel) && !(h->index & 3));
        uint32_t v = h->intel[h->index / 4]; if (h->index == 0xc0) h->intel[h->index / 4] = 0; return v;
    }
    if (h->kind == S_PCNET) {
        if (r == 0x10) { unsigned s = h->csr[h->index]; return h->index == 0 && (s & PC_IRQS) ? s | 0x80 : s; }
        if (r == 0x16) return h->index == 34 ? h->phy_regs[h->bcr[33] & 31] : h->bcr[h->index];
        if (r == 0x14) { if (!h->reset_stuck) h->csr[0] = 4; return 0; }
    }
    if (h->kind == S_TULIP && r == 0x48) return (h->ee_pins & 0x800) ? h->ee_pins | (h->ee_sample ? 8 : 0) : h->mii_pins | (h->mii_sample ? 0x80000 : 0);
    if (h->kind == S_3COM) {
        if (r == 0x0e) return (h->window << 13) | (h->command ? 0x1000 : 0) | model_get(h->fixture.reg + r, 2);
        if (r == 0x1b) return h->tx_status;
        if (r < 14) {
            if (h->window == 0 && r == 10) return h->ee_stuck ? 0x8000 : 0;
            if (h->window == 0 && r == 12) return (unsigned)second_mac[(h->ee_address - 10) * 2] << 8 | second_mac[(h->ee_address - 10) * 2 + 1];
            if (h->window == 4 && r == 8) return h->mii_pins | (h->mii_sample ? 2 : 0);
            return model_get(h->windows[h->window] + r, width);
        }
    }
    if (h->kind == S_RHINE && r == 0x70 && (h->mii_pins & 0x10))
        return h->mii_pins | (h->mii_sample ? 2 : 0);
    return model_get(h->fixture.reg + r, width);
}
static void second_write(void *arg, uint16_t port, uint32_t v, unsigned width)
{
    struct second_model *h = arg; struct netif *d = h->fixture.device;
    if (port == 0xcf8 || port == 0xcfc) {
        if (port == 0xcfc && (v & 4)) second_dma_ready(h);
        model_write(&h->fixture, port, v, width); return;
    }
    CHECK(port >= 0x1000 && port < 0x1100); unsigned r = port - 0x1000;
    CHECK(d->claims[0] >= 0 && registry_valid(d->claims[0], d->claim_generation[0])); h->nic_writes++;
    if (h->kind == S_E1000) {
        CHECK(width == 4); if (!r) { h->index = v; return; }
        CHECK(r == 4 && h->index < sizeof(h->intel) && !(h->index & 3)); r = h->index;
        if (!r && (v & (1u << 26))) {
            if (!h->reset_stuck) { h->intel[0] = 0; h->intel[0x100 / 4] = 0; h->intel[0x400 / 4] = 8; h->intel[0x5404 / 4] &= ~NIC_OWN; }
            else h->intel[0] = v;
            return;
        }
        h->intel[r / 4] = v;
        if (r == 0x14 && (v & 1) && !h->ee_stuck) {
            unsigned address = (v >> 8) & 0xff; CHECK(address < 3);
            unsigned word = second_mac[address * 2] | (unsigned)second_mac[address * 2 + 1] << 8;
            h->intel[r / 4] = (word << 16) | 0x10;
        }
        if (r == 0x20 && !h->mii_stuck) {
            unsigned phy=(v>>21)&31,reg=(v>>16)&31,op=(v>>26)&3;
            if(phy!=1)h->intel[r/4]=1u<<30;
            else {
                if(op==1) {
                    h->phy_regs[reg]=(uint16_t)v;
                    if(reg==0 && (v&0x8000)) { h->phy_regs[16]&=(uint16_t)~0x800u;h->phy_regs[20]&=(uint16_t)~0x70u; }
                }
                if(reg==0 && !h->phy_reset_stuck)h->phy_regs[0]&=(uint16_t)~0x8000u;
                h->intel[r/4]=(1u<<28)|h->phy_regs[reg];
            }
        }
        if (r == 0x3818 && d->tx_used[0]) second_tx(h);
        return;
    }
    if (h->kind == S_PCNET) {
        if (r == 0x12) { CHECK(v < 128 && width == 2); h->index = v; return; }
        if (r == 0x16) {
            if (h->index == 34) h->phy_regs[h->bcr[33] & 31] = v;
            else if (h->index == 4) h->bcr[4] = (h->bcr[4] & 0x8000) | (v & 0x7fff);
            else h->bcr[h->index] = v;
            return;
        }
        if (r == 0x10) {
            if (h->index) { h->csr[h->index] = v; return; }
            h->csr[0] &= ~(v & PC_IRQS); h->csr[0] = (h->csr[0] & ~0x40) | (v & 0x40);
            if (v & 4) { if (!h->stop_stuck) h->csr[0] = 4; }
            if (v & 1) { CHECK(h->dma_prepared); if (!h->init_stuck) h->csr[0] = 0x100; }
            if (v & 2) h->csr[0] = 0x32;
            if (v & 8) second_tx(h);
            return;
        }
    }
    if (h->kind == S_TULIP) {
        if (r == 0 && (v & 1)) { model_put(h->fixture.reg, h->reset_stuck ? 1 : 0, 4); model_put(h->fixture.reg + 0x28, 0, 4); return; }
        if (r == 0x28) { model_put(h->fixture.reg + r, model_get(h->fixture.reg + r, 4) & ~v, 4); return; }
        if (r == 0x48) {
            if (v & 0x800) second_ee_clock(h, v);
            else { h->ee_pins = 0; second_mii_clock(h, v); }
            return;
        }
        if (r == 0x30) {
            unsigned s = model_get(h->fixture.reg + 0x28, 4) & ~0x7e0000;
            if (h->stop_stuck || (v & 0x2000)) s |= 6u << 20;
            if (h->stop_stuck || (v & 2)) s |= 3u << 17;
            model_put(h->fixture.reg + 0x28, s, 4);
            if (v & 0x2000) {
                CHECK(h->dma_prepared);
                unsigned base = model_get(h->fixture.reg + 0x20, 4);
                if (base == d->dma_phys + 512) {
                    struct nic_desc *sdesc = model_dma(base, 16);
                    const uint8_t *setup = model_dma(sdesc->word[2], 192);
                    CHECK(!memcmp(setup, second_mac, 2) && setup[180] == 0xff && setup[181] == 0xff);
                    if (!h->init_stuck) sdesc->word[0] = 0x7fffffff; /* QEMU's undefined setup status */
                } else second_tx(h);
            }
        }
        if (r == 8 && (model_get(h->fixture.reg + 0x20, 4) == d->dma_phys + NIC_TX_DESC)) second_tx(h);
    }
    if (h->kind == S_3COM) {
        if (r == 0x0e) {
            if ((v & 0xf800) == 0x0800) { h->window = v & 7; return; }
            if (!v) { if (h->reset_stuck || h->stop_stuck) h->command = 1; else { memset(h->windows, 0, sizeof(h->windows)); memset(h->fixture.reg, 0, sizeof(h->fixture.reg)); h->window = 0; } return; }
            if ((v & 0xf800) == 0x6800) { model_put(h->fixture.reg + r, model_get(h->fixture.reg + r, 2) & ~(v & 0x7ff), 2); return; }
            if (v == 0x2000) model_put(h->windows[4] + 6, model_get(h->windows[4] + 6, 2) | 0x400, 2);
            if (v == 0x4800) model_put(h->windows[4] + 6, model_get(h->windows[4] + 6, 2) | 0x800, 2);
            if (v == 0x1800) model_put(h->windows[4] + 6, model_get(h->windows[4] + 6, 2) & ~0x400, 2);
            if (v == 0x5000) model_put(h->windows[4] + 6, model_get(h->windows[4] + 6, 2) & ~0x800, 2);
            return;
        }
        if (r == 0x1b) { h->tx_status = 0; model_put(h->fixture.reg + 0x0e, model_get(h->fixture.reg + 0x0e, 2) & ~4, 2); return; }
        if (r < 14) {
            if (h->window == 0 && r == 10) { h->ee_address = v & 63; return; }
            if (h->window == 4 && r == 8) { second_mii_clock(h, v); return; }
            model_put(h->windows[h->window] + r, v, width); return;
        }
        model_put(h->fixture.reg + r, v, width); if (r == 0x24 && v) second_tx(h); return;
    }
    if (h->kind == S_RHINE) {
        if (r == 9 && (v & 0x80)) { if (!h->reset_stuck) { h->fixture.reg[8] = 4; h->fixture.reg[9] = 0; } else h->fixture.reg[9] = 0x80; return; }
        if (r == 8 && v == 4 && h->stop_stuck) return;
        if (r == 0x0c) { model_put(h->fixture.reg + r, model_get(h->fixture.reg + r, 2) & ~v, 2); return; }
        if (r == 0x71) { h->fixture.reg[r] = v | 0x80; return; }
        if (r == 0x70) {
            if (v & 0x10) { second_mii_clock(h, v); return; }
            h->fixture.reg[r] = h->mii_stuck ? v : 0;
            unsigned reg = h->fixture.reg[0x71] & 31;
            if (v & 0x40) { model_put(h->fixture.reg + 0x72, h->phy_regs[reg], 2); h->mii_reads++; }
            if (v & 0x20) h->phy_regs[reg] = model_get(h->fixture.reg + 0x72, 2);
            return;
        }
        if (r == 8 && (v & 0x20)) second_tx(h);
    }
    model_put(h->fixture.reg + r, v, width);
}
static int second_delay(void *arg, uint32_t us)
{
    struct second_model *h = arg; CHECK(host_flags & 0x200); h->delay_calls++; cycles += us;
    return h->mii_stuck && (h->kind == S_TULIP || h->kind == S_3COM) ? -110 : 0;
}
static uint32_t second_mmio_read(void *arg, volatile void *mapped, unsigned reg)
{
    struct second_model *h = arg; CHECK(mapped == &mmio_token && h->kind == S_E1000);
    h->index = reg; return second_read(h, 0x1004, 4);
}
static void second_mmio_write(void *arg, volatile void *mapped, unsigned reg, uint32_t value)
{
    struct second_model *h = arg; CHECK(mapped == &mmio_token && h->kind == S_E1000);
    h->index = reg; second_write(h, 0x1004, value, 4);
}
static void second_setup(struct second_model *h, struct netif *d, enum second_kind kind)
{
    memset(h, 0, sizeof(*h)); model_setup(&h->fixture, d, M_RTL); h->kind = kind;
    h->phy_address = kind == S_3COM ? 24 : 1; h->phy_regs[1] = 0x782c;
    h->tx_auto = h->wire_done = true; h->ee_bits = 6; h->ee_sample = 1;
    memset(h->fixture.reg, 0, sizeof(h->fixture.reg)); memcpy(h->fixture.reg, second_mac, 6);
    const uint16_t vendors[] = {0x8086,0x1022,0x1011,0x10b7,0x1106};
    const uint16_t ids[] = {0x100e,0x2000,0x0019,0x9055,0x3065};
    functions[1].vendor = vendors[kind]; functions[1].device = ids[kind];
    if (kind == S_E1000) {
        functions[1].bar[0] = 0xf0000000; functions[1].command = 2;
        model_put(h->fixture.pci + 4, 0xabcd0002, 4);
        h->intel[8 / 4] = 2; h->intel[0x5400 / 4] = 0x12005452; h->intel[0x5404 / 4] = 0x80005634;
    } else if (kind == S_PCNET) {
        unsigned chip = (0x2430u << 12) | 3; h->csr[88] = chip; h->csr[89] = chip >> 16; h->csr[0] = 4; h->bcr[4] = 0x8000;
    } else if (kind == S_TULIP || kind == S_3COM) memset(h->fixture.reg, 0, 8);
    else { h->fixture.reg[8] = 4; h->fixture.reg[0x71] = 0x80; h->fixture.reg[0x6c] = 1; }
}
static int second_attach(struct second_model *h, struct netif *d)
{
    struct net_io io = {h,second_read,second_write,model_now,model_sleep,second_delay,second_mmio_read,second_mmio_write};
    return netif_attach(d, &functions[1], &io);
}
static void second_receive(struct second_model *h, const uint8_t *frame, unsigned len, bool error)
{
    struct netif *d = h->fixture.device; unsigned i = h->rx_cursor;
    memcpy(nic_buffer(d, false, i), frame, len); struct nic_desc *r = nic_desc_at(d, false, i);
    if (h->kind == S_E1000) { struct e1_rx *e = (void *)r; CHECK(!e->status); e->length = len; e->errors = error; e->status = 3; h->intel[0xc0 / 4] |= 0x80; }
    if (h->kind == S_PCNET) { CHECK(r->word[1] & NIC_OWN); r->word[1] = 0x0300f800 | (error ? 0x40000000 : 0); r->word[2] = len + 4; h->csr[0] |= 0x400; }
    if (h->kind == S_TULIP) { CHECK(r->word[0] & NIC_OWN); r->word[0] = ((len + 4) << 16) | 0x300 | (error ? 0x8000 : 0); model_put(h->fixture.reg + 0x28, model_get(h->fixture.reg + 0x28, 4) | 0x40, 4); }
    if (h->kind == S_3COM) { CHECK(!r->word[1]); r->word[1] = 0x8000 | len | (error ? 0x4000 : 0); model_put(h->fixture.reg + 0x0e, model_get(h->fixture.reg + 0x0e, 2) | 0x400, 2); }
    if (h->kind == S_RHINE) { CHECK(r->word[0] & NIC_OWN); r->word[0] = ((len + 4) << 16) | 0x8300 | (error ? 1 : 0); h->fixture.reg[0x0c] |= 1; }
    h->rx_cursor = (i + 1) % NIC_RX_COUNT;
}
static void second_link(struct second_model *h, bool up)
{
    if (h->kind == S_E1000) h->intel[2] = up ? 2 : 0;
    else if (h->kind == S_PCNET && h->fixture.device->media_control == 0x2430) h->bcr[4] = up ? 0x8040 : 0x40;
    else h->phy_regs[1] = up ? 0x782c : 0x7828;
}
static void second_bad_descriptor(struct second_model *h, bool pointer)
{
    struct netif *d = h->fixture.device; struct nic_desc *r = nic_desc_at(d, false, d->rx_head);
    if (pointer) r->word[h->kind == S_E1000 || h->kind == S_PCNET ? 0 : 2]++;
    else if (h->kind == S_E1000) ((struct e1_rx *)r)->length = 0xffff;
    else if (h->kind == S_PCNET) r->word[2] = 0xfff;
    else if (h->kind == S_3COM) r->word[1] |= 0x1fff;
    else r->word[0] |= 0x07ff0000;
}
static void test_second_paths(enum second_kind kind)
{
    struct second_model h; struct netif d; second_setup(&h, &d, kind);
    CHECK(!second_attach(&h, &d) && d.running && d.irq_attached && h.dma_prepared);
    CHECK(!memcmp(d.address, second_mac, 6) && d.link == NET_LINK_UP);
    uint8_t frame[NET_FRAME_MAX]; for (unsigned i = 0; i < sizeof(frame); i++) frame[i] = i;
    for (unsigned n = 0; n < 80; n++) {
        unsigned len = n % 2 ? NET_FRAME_MAX : 61;
        CHECK(!netif_transmit(&d, d.generation, frame, n ? len : 42)); netif_poll(&d);
        CHECK(d.stats.tx_completed == n + 1 && !d.quarantined);
        second_receive(&h, frame, len, false);
        unsigned packet_io = h.fixture.packet_io, delay = h.delay_calls, slept = sleeps, pages = page_count;
        uint32_t f = irq_save(); CHECK(netif_irq(&d)); irq_restore(f);
        CHECK(h.fixture.packet_io == packet_io && h.delay_calls == delay && sleeps == slept && page_count == pages);
        netif_poll(&d); struct net_packet p; CHECK(netif_pop(&d, &p)); CHECK(p.length == len && !memcmp(p.data, frame, len));
    }
    second_receive(&h, frame, 60, true); netif_poll(&d); CHECK(d.stats.rx_errors == 1 && !d.quarantined && !d.queue_count);
    second_link(&h, false); g_ticks += 1000; netif_poll(&d);
    CHECK(d.link == NET_LINK_DOWN);
    second_link(&h, true); g_ticks += 1000; netif_poll(&d); CHECK(d.link == NET_LINK_UP);
    uint32_t f = irq_save(); CHECK(!netif_irq(&d)); irq_restore(f); /* peer's shared IRQ */
    for (unsigned i = 0; i < NET_QUEUE_MAX + 3; i++) { second_receive(&h, frame, 60, false); netif_poll(&d); }
    CHECK(d.queue_count == NET_QUEUE_MAX && d.stats.rx_dropped == 3 && d.stats.queue_peak == NET_QUEUE_MAX);
    CHECK(!netif_stop(&d) && !page_count && releases == 3 && irq_removes == 1);
    CHECK(kind != S_E1000 || (map_calls == 1 && unmap_calls == 1 && !d.mmio));
}
static void test_second_faults(enum second_kind kind)
{
    struct second_model h; struct netif d; uint8_t frame[60] = {0};
    second_setup(&h, &d, kind); CHECK(!second_attach(&h, &d)); h.tx_auto = false;
    unsigned slots = kind == S_E1000 || kind == S_3COM ? 1 : NIC_TX_COUNT;
    for (unsigned i = 0; i < slots; i++) CHECK(!netif_transmit(&d, d.generation, frame, sizeof(frame)));
    CHECK(netif_transmit(&d, d.generation, frame, sizeof(frame)) == -16);
    g_ticks += NET_TX_TIMEOUT_MS; netif_poll(&d); CHECK(d.quarantined && d.stats.tx_errors && !releases && page_count == NIC_DMA_PAGES);
    unsigned writes = h.nic_writes; CHECK(netif_stop(&d) && writes == h.nic_writes && page_count == NIC_DMA_PAGES);
    CHECK(!(model_get(h.fixture.pci + 4, 2) & 4));
    for (unsigned pointer = 0; pointer < 2; pointer++) {
        second_setup(&h, &d, kind); CHECK(!second_attach(&h, &d)); second_receive(&h, frame, 60, false);
        second_bad_descriptor(&h, pointer); netif_poll(&d); CHECK(d.quarantined && !d.queue_count && !releases);
    }
    second_setup(&h, &d, kind); CHECK(!second_attach(&h, &d));
    if (kind == S_E1000) h.reset_stuck = true; else h.stop_stuck = true;
    CHECK(netif_stop(&d) && d.quarantined && page_count == NIC_DMA_PAGES && !releases);
    second_setup(&h, &d, kind); CHECK(!second_attach(&h, &d)); h.absent = true;
    netif_poll(&d); CHECK(d.quarantined && !releases);
    second_setup(&h, &d, kind); CHECK(!second_attach(&h, &d)); h.fixture.read_cost = 60;
    for (unsigned i = 0; i < IRQ_BUDGET_STRIKES; i++) {
        second_receive(&h, frame, 60, false);
        uint32_t f = irq_save(); CHECK(netif_irq(&d)); irq_restore(f);
        CHECK(d.fault_pending == (i + 1 == IRQ_BUDGET_STRIKES));
    }
    CHECK(d.fault_pending && d.stats.budget_violations == IRQ_BUDGET_STRIKES);
    netif_poll(&d); CHECK(d.quarantined && !releases);
    second_setup(&h, &d, kind); CHECK(!second_attach(&h, &d)); h.tx_error = true;
    CHECK(!netif_transmit(&d, d.generation, frame, sizeof(frame))); netif_poll(&d); CHECK(d.stats.tx_errors == 1 && !d.stats.tx_completed);
}
static void test_second_admission(enum second_kind kind)
{
    struct second_model h; struct netif d;
    second_setup(&h, &d, kind); g_boot.flags |= CBI_F_SAFE_MODE;
    CHECK(second_attach(&h, &d) == -19 && !h.nic_writes && !config_writes && !page_count);
    for (unsigned fail = 1; fail <= 3; fail++) {
        second_setup(&h, &d, kind); claim_fail = fail;
        if(fail==2) {
            CHECK(!second_attach(&h,&d) && d.running && !d.irq_attached && !d.quarantined);
            CHECK(!netif_stop(&d) && !page_count);
        } else CHECK(second_attach(&h, &d) == -16 && !h.nic_writes && !config_writes && !page_count);
    }
    second_setup(&h, &d, kind); allocation_fail = 1;
    CHECK(second_attach(&h, &d) == -ENOMEM && !h.nic_writes && !config_writes && !page_count);
    second_setup(&h, &d, kind); h.absent = true;
    CHECK(second_attach(&h, &d) == -16 && !config_writes && !page_count);
    second_setup(&h, &d, kind);
    if (kind == S_E1000) h.intel[0x100 / 4] = 2;
    if (kind == S_PCNET) h.csr[0] = 0x32;
    if (kind == S_TULIP) model_put(h.fixture.reg + 0x30, TU_RUN, 4);
    if (kind == S_3COM) model_put(h.windows[4] + 6, 0xc00, 2);
    if (kind == S_RHINE) h.fixture.reg[8] = 0x1a;
    CHECK(second_attach(&h, &d) == -16 && !config_writes && !page_count && !d.touched);
    second_setup(&h, &d, kind); g_boot.flags = 0;
    CHECK(!second_attach(&h, &d) && !d.irq_attached); CHECK(!netif_stop(&d));
    second_setup(&h, &d, kind); irq_add_fail = 1;
    CHECK(!second_attach(&h, &d) && d.running && !d.quarantined && !d.irq_attached);
    CHECK(!netif_stop(&d) && !page_count);
}
static void test_second_variants(void)
{
    struct second_model h; struct netif d; uint8_t frame[60] = {0};
    second_setup(&h, &d, S_E1000); functions[1].bar[0] |= 4; functions[1].bar[1] = 0;
    CHECK(!second_attach(&h, &d)); CHECK(!netif_stop(&d)); /* supported 64-bit BAR below 4 GiB */
    second_setup(&h, &d, S_E1000); functions[1].bar[0] |= 4; functions[1].bar[1] = 1;
    CHECK(second_attach(&h, &d) == -19 && !h.nic_writes && !config_writes && !map_calls);
    second_setup(&h, &d, S_E1000); map_fail = true;
    CHECK(second_attach(&h, &d) == -ENOMEM && !page_count && !h.nic_writes && !config_writes && releases == 2);
    second_setup(&h, &d, S_E1000); CHECK(!second_attach(&h, &d));
    unsigned writes = h.nic_writes; net_write(&d, 0x20000, 1, 4);
    CHECK(d.fault_pending && h.nic_writes == writes); netif_poll(&d); CHECK(d.quarantined && !unmap_calls);
    second_setup(&h, &d, S_E1000); h.ee_stuck = true;
    CHECK(second_attach(&h, &d) == -110 && d.quarantined && !unmap_calls && sleeps <= 35);
    second_setup(&h, &d, S_PCNET); unsigned chip = (0x2625u << 12) | 3;
    h.csr[88] = chip; h.csr[89] = chip >> 16; h.bcr[33] = 1 << 5;
    CHECK(!second_attach(&h, &d)); CHECK(d.link == NET_LINK_UP); CHECK(!netif_stop(&d));
    second_setup(&h, &d, S_TULIP); h.ee_bits = 8; functions[1].device = 0x0009;
    CHECK(!second_attach(&h, &d) && d.eeprom_bits == 8); CHECK(!netif_stop(&d));
    second_setup(&h, &d, S_TULIP); h.qemu_mii_edge = true;
    CHECK(!second_attach(&h, &d) && d.phy == 1 && d.link == NET_LINK_UP);
    second_link(&h, false); CHECK(d.driver->link_state(&d) == NET_LINK_DOWN); CHECK(!netif_stop(&d));
    second_setup(&h, &d, S_TULIP); h.init_stuck = true;
    CHECK(second_attach(&h, &d) == -110 && d.quarantined && sleeps <= 102);
    second_setup(&h, &d, S_PCNET); h.init_stuck = true;
    CHECK(second_attach(&h, &d) == -110 && d.quarantined && sleeps <= 100);
    second_setup(&h, &d, S_3COM); h.ee_stuck = true;
    CHECK(second_attach(&h, &d) == -110 && d.quarantined && sleeps <= 100);
    second_setup(&h, &d, S_RHINE); h.mii_stuck = true;
    CHECK(second_attach(&h, &d) == -110 && d.quarantined && h.delay_calls <= 100);
    second_setup(&h, &d, S_RHINE); functions[1].device = 0x3043;
    CHECK(!second_attach(&h, &d) && d.link == NET_LINK_UP); CHECK(!netif_stop(&d));
    second_setup(&h, &d, S_RHINE); functions[1].device = 0x3106;
    CHECK(!second_attach(&h, &d) && d.phy == 1); CHECK(!netif_stop(&d));
    second_setup(&h, &d, S_3COM); CHECK(!second_attach(&h, &d)); h.wire_done = false;
    CHECK(!netif_transmit(&d, d.generation, frame, 60)); netif_poll(&d);
    CHECK(d.tx_used[0] && !d.stats.tx_completed); /* DMA retirement alone is insufficient */
    h.tx_status = 0xc0; netif_poll(&d); CHECK(d.stats.tx_completed == 1); CHECK(!netif_stop(&d));
    puts("PASS second set: e1000/PCnet/Tulip/3c905/Rhine DMA/register models, admission, rings, IRQ, link, errors, timeout/quarantine");
}
static struct second_model *second_probe_hardware;
static void second_probe_step(void)
{
    struct netif *d = second_probe_hardware->fixture.device;
    if (d->mutex.owner) return;
    if (inject_reply && !reply_injected) {
        uint8_t frame[60], peer[6] = {0x52,0x55,10,0,2,2}; net_arp_request(frame, d->address);
        memcpy(frame, d->address, 6); memcpy(frame + 6, peer, 6); frame[21] = 2;
        memcpy(frame + 22, peer, 6); memcpy(frame + 32, d->address, 6);
        frame[31] = 2; frame[41] = wrong_target ? 16 : 15;
        second_receive(second_probe_hardware, frame, sizeof(frame), false); reply_injected = true;
        uint32_t flags = irq_save(); CHECK(netif_irq(d)); irq_restore(flags);
    }
    net_worker_step();
}
static void test_second_probe(enum second_kind kind)
{
    for (unsigned scenario = 0; scenario < 3; scenario++) {
        struct second_model h; struct netif *d = &devices[0]; second_setup(&h, d, kind);
        CHECK(!second_attach(&h, d)); device_count = 1; second_probe_hardware = &h;
        inject_reply = scenario != 1; reply_injected = false; wrong_target = scenario == 2;
        record_count = 0; terminal_pass = false; memset(records, 0, sizeof(records)); sleep_hook = second_probe_step;
        CHECK(probe_net_arp() == (scenario ? -5 : 0)); CHECK(terminal_pass == !scenario);
        CHECK(!page_count && !d->irq_attached && !d->running);
        sleep_hook = 0; device_count = 0;
    }
}
static void test_second_set(void)
{
    for (enum second_kind kind = S_E1000; kind <= S_RHINE; kind++) {
        test_second_paths(kind); test_second_faults(kind); test_second_admission(kind);
    }
    test_second_variants();
    for (enum second_kind kind = S_E1000; kind <= S_RHINE; kind++) test_second_probe(kind);
}
