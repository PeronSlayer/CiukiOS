/* DP8390D pages 0/1/2, NE2000 word PIO and private packet ring.
 * References/intentional deviations: docs/validation/f6-11-dos-network.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "dosvm_ne2000.h"

static void irq_update(struct dosvm_ne2000 *d)
{
    bool level = (d->isr & d->imr & 0x7f) != 0;
    if (level != d->irq_level) { d->irq_level = level; if (d->ops.irq) d->ops.irq(d->ops.arg, level); }
}
void dosvm_ne2000_reset(struct dosvm_ne2000 *d)
{
    if (d->irq_level && d->ops.irq) d->ops.irq(d->ops.arg, false);
    d->irq_level = false;
    d->command = 0x21; d->isr = 0x80; d->imr = 0;
    d->pstart = d->pstop = d->boundary = d->current = 0;
    d->remote = d->remote_start = d->count = d->tx_count = d->local = d->address_count = 0;
    d->rcr = d->tcr = d->dcr = d->tsr = d->rsr = d->tpsr = d->fifo = 0;
    d->local_next = d->remote_next = 0; memset(d->counters, 0, sizeof(d->counters));
    memcpy(d->par, d->assigned, 6); memset(d->mar, 0, sizeof(d->mar));
    if (!++d->generation) ++d->generation;
}
void dosvm_ne2000_init(struct dosvm_ne2000 *d, const uint8_t mac[6], const struct dosvm_ne2000_ops *ops)
{
    memset(d, 0, sizeof(*d)); if (ops) d->ops = *ops;
    memcpy(d->assigned, mac, 6);
    /* NE2000 PROM duplicates each byte on the 16-bit data bus. */
    for (unsigned i = 0; i < 16; i++) d->prom[2*i] = d->prom[2*i+1] = i < 6 ? mac[i] : i >= 14 ? 0x57 : 0;
    dosvm_ne2000_reset(d);
}
static bool ring_valid(const struct dosvm_ne2000 *d)
{
    return d->pstart >= 0x40 && d->pstop <= 0x80 && d->pstart + 1 < d->pstop &&
        d->boundary >= d->pstart && d->boundary < d->pstop &&
        d->current >= d->pstart && d->current < d->pstop;
}
static uint8_t memory_read(struct dosvm_ne2000 *d, unsigned at)
{ return at < 32 ? d->prom[at] : at >= 0x4000 && at < 0x8000 ? d->ram[at - 0x4000] : 0xff; }
static void dma_bad(struct dosvm_ne2000 *d)
{ d->bad_dma++; d->count = 0; d->isr |= 0x44; irq_update(d); }
static uint32_t dma(struct dosvm_ne2000 *d, unsigned width, bool write, uint32_t value)
{
    uint32_t out = width == 1 ? 0xff : 0xffff;
    unsigned mode = d->command & 0x38;
    if ((d->command & 1) || !d->count || (write ? mode != 0x10 : mode != 8)) return out;
    /* DCR.WTS determines the bus transfer size, as in QEMU's byte/word ASIC
     * port. No 32-bit NE2000 PIO is exposed by this ISA model. */
    unsigned step = (d->dcr & 1) ? 2 : 1;
    unsigned at = step == 2 ? d->remote & ~1u : d->remote;
    if (!((!write && at + step <= 32) || (at >= 0x4000 && at + step <= 0x8000))) {
        dma_bad(d); return out;
    }
    out = 0;
    for (unsigned i = 0; i < step; i++) {
        if (write) d->ram[at + i - 0x4000] = (uint8_t)(value >> (i * 8));
        else out |= (uint32_t)memory_read(d, at + i) << (i * 8);
    }
    unsigned next = d->remote + step;
    if (ring_valid(d) && d->remote >= (unsigned)d->pstart << 8 && next == (unsigned)d->pstop << 8)
        next = (unsigned)d->pstart << 8;
    if (next > 0xffff) { dma_bad(d); return out; }
    d->remote = (uint16_t)next;
    d->count = d->count > step ? d->count - step : 0;
    if (!d->count) { d->isr |= 0x40; irq_update(d); }
    return width == 1 ? out & 255 : out;
}
static void transmit(struct dosvm_ne2000 *d)
{
    unsigned start = (unsigned)d->tpsr << 8, n = d->tx_count;
    bool ok = start >= 0x4000 && start < 0x8000 && n >= 14 && n <= 1514 && n <= 0x8000 - start;
    if (ok && (d->tcr & 6)) ok = dosvm_ne2000_receive(d, d->ram + start - 0x4000, n) > 0;
    else if (ok) ok = d->ops.send && !d->ops.send(d->ops.arg, d->ram + start - 0x4000, n);
    d->tsr = ok ? 1 : 8; d->isr |= ok ? 2 : 8;
    d->command &= ~4u; irq_update(d);
}
static uint8_t register_read(struct dosvm_ne2000 *d, unsigned reg)
{
    if (!reg) return d->command;
    switch (d->command >> 6) {
    case 0:
        switch (reg) {
        case 1: return (uint8_t)d->local; case 2: return d->local >> 8;
        case 3: return d->boundary; case 4: return d->tsr; case 5: return 0;
        case 6: return d->fifo; case 7: return d->isr;
        case 8: return (uint8_t)d->remote; case 9: return d->remote >> 8;
        case 12: return d->rsr;
        case 13: case 14: case 15: { uint8_t n = d->counters[reg - 13]; d->counters[reg - 13] = 0; return n; }
        default: return 0;
        }
    case 1:
        if (reg <= 6) return d->par[reg - 1];
        return reg == 7 ? d->current : d->mar[reg - 8];
    case 2:
        switch (reg) {
        case 1: return d->pstart; case 2: return d->pstop;
        case 3: return d->remote_next; case 4: return d->tpsr;
        case 5: return d->local_next; case 6: return d->address_count >> 8;
        case 7: return (uint8_t)d->address_count;
        case 12: return d->rcr; case 13: return d->tcr; case 14: return d->dcr; case 15: return d->imr;
        default: return 0;
        }
    default: return 0; /* Reserved page 3, no PCI RTL8029 extensions. */
    }
}
uint32_t dosvm_ne2000_read(struct dosvm_ne2000 *d, uint16_t port, unsigned width)
{
    if (!d || port < 0x300 || port > 0x31f || (width != 1 && width != 2)) return UINT32_MAX;
    unsigned reg = port - 0x300;
    if (reg == 0x10) return dma(d, width, false, 0);
    if (width != 1) return 0xffff;
    if (reg == 0x1f) { dosvm_ne2000_reset(d); return 0; }
    return reg < 16 ? register_read(d, reg) : 0xff;
}
void dosvm_ne2000_write(struct dosvm_ne2000 *d, uint16_t port, unsigned width, uint32_t value)
{
    if (!d || port < 0x300 || port > 0x31f || (width != 1 && width != 2)) return;
    unsigned reg = port - 0x300; uint8_t n = (uint8_t)value;
    if (reg == 0x10) { (void)dma(d, width, true, value); return; }
    if (width != 1 || reg >= 16) return; /* Reset write acknowledges no new event. */
    if (!reg) {
        d->command = n;
        if (n & 1) d->isr |= 0x80;
        else {
            d->isr &= ~0x80u;
            if ((n & 0x38) == 8 && !d->count) d->isr |= 0x40;
            if (n & 4) transmit(d);
        }
        irq_update(d); return;
    }
    switch (d->command >> 6) {
    case 0:
        switch (reg) {
        case 1: d->pstart = n; break; case 2: d->pstop = n; break;
        case 3: d->boundary = n; break; case 4: d->tpsr = n; break;
        case 5: d->tx_count = (d->tx_count & 0xff00) | n; break;
        case 6: d->tx_count = (d->tx_count & 255) | (uint16_t)n << 8; break;
        case 7: d->isr &= ~(n & 0x7f); break;
        case 8: d->remote_start = (d->remote_start & 0xff00) | n; d->remote = d->remote_start; break;
        case 9: d->remote_start = (d->remote_start & 255) | (uint16_t)n << 8; d->remote = d->remote_start; break;
        case 10: d->count = (d->count & 0xff00) | n; break;
        case 11: d->count = (d->count & 255) | (uint16_t)n << 8; break;
        case 12: d->rcr = n; break; case 13: d->tcr = n; break;
        case 14: d->dcr = n; break; case 15: d->imr = n & 0x7f; break;
        }
        break;
    case 1:
        if (reg <= 6) d->par[reg - 1] = n;
        else if (reg == 7) d->current = n;
        else d->mar[reg - 8] = n;
        break;
    case 2:
        switch (reg) {
        case 1: d->local = (d->local & 0xff00) | n; break;
        case 2: d->local = (d->local & 255) | (uint16_t)n << 8; break;
        case 3: d->remote_next = n; break; case 5: d->local_next = n; break;
        case 6: d->address_count = (d->address_count & 255) | (uint16_t)n << 8; break;
        case 7: d->address_count = (d->address_count & 0xff00) | n; break;
        }
        break;
    }
    irq_update(d);
}
void dosvm_ne2000_overflow(struct dosvm_ne2000 *d)
{ d->dropped++; d->rsr = 0x10; d->isr |= 0x10; irq_update(d); }
static uint32_t crc32(const uint8_t *data, unsigned n)
{
    uint32_t crc = UINT32_MAX;
    for (unsigned i = 0; i < n; i++) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
int dosvm_ne2000_receive(struct dosvm_ne2000 *d, const uint8_t *data, unsigned n)
{
    if (!d || !data || n < 14 || n > 1514) return -EINVAL;
    if ((d->command & 1) || !(d->command & 2) || (d->rcr & 0x20)) return 0;
    static const uint8_t all[6] = {255,255,255,255,255,255};
    if (!(d->rcr & 0x10)) {
        if (!memcmp(data, all, 6)) { if (!(d->rcr & 4)) return 0; }
        else if (data[0] & 1) {
            unsigned bit = 0; uint32_t crc = UINT32_MAX;
            for (unsigned i = 0; i < 6; i++) for (unsigned j = 0; j < 8; j++) {
                bool carry = ((crc >> 31) ^ (data[i] >> j)) & 1;
                crc <<= 1; if (carry) crc ^= 0x04c11db7;
            }
            bit = crc >> 26;
            if (!(d->rcr & 8) || !(d->mar[bit / 8] & (1u << (bit % 8)))) return 0;
        } else if (memcmp(data, d->par, 6)) return 0;
    }
    if (!ring_valid(d)) { dosvm_ne2000_overflow(d); return -ENOSPC; }
    unsigned length = n < 60 ? 60 : n;
    unsigned pages = (length + 8 + 255) / 256;
    unsigned available = d->boundary > d->current ? d->boundary - d->current :
        d->pstop - d->current + d->boundary - d->pstart;
    if (pages >= available) { dosvm_ne2000_overflow(d); return -ENOSPC; }
    unsigned next = d->current + pages;
    if (next >= d->pstop) next -= d->pstop - d->pstart;
    uint8_t status = (data[0] & 1) ? 0x21 : 1;
    unsigned at = (unsigned)d->current << 8;
    uint8_t header[4] = {status, (uint8_t)next, (uint8_t)(length + 4), (uint8_t)((length + 4) >> 8)};
    /* Compute CRC over the padded frame without a large stack copy. */
    uint32_t crc = crc32(data, n);
    if (length > n) {
        crc = ~crc;
        for (unsigned i = n; i < length; i++) for (unsigned j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
        crc = ~crc;
    }
    for (unsigned i = 0; i < length + 8; i++) {
        uint8_t byte = i < 4 ? header[i] : i < n + 4 ? data[i - 4] :
            i < length + 4 ? 0 : (uint8_t)(crc >> ((i - length - 4) * 8));
        d->ram[at - 0x4000] = byte;
        if (++at == (unsigned)d->pstop << 8) at = (unsigned)d->pstart << 8;
    }
    d->current = next; d->rsr = status; d->isr |= 1; irq_update(d); return 1;
}
