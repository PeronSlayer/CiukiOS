/* Per-VM virtual 8237A pair: slave 00h-0Fh (channels 0-3, bytes), master
 * C0h-DEh even ports (channels 4-7, words), page registers 80h-8Fh.
 * Intel 8237A datasheet 231466-005 (registers, software commands, TC and
 * autoinitialize); page map and word addressing as in the PC/AT and QEMU
 * i8257. Guest DMA addresses resolve only through this VM's checked memory:
 * an address outside it fails locally. No physical DMA is ever programmed.
 * Research: docs/design/f3-acceptance.md, f3-05. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include "dosvm_internal.h"

/* Page register (offset from 80h) of each channel; channel 4 is the cascade. */
static const uint8_t dma_page_of[8] = {7, 3, 1, 2, 0xF, 0xB, 9, 0xA};

void dosvm_dma_init(struct dosvm *v)
{
    struct dosvm_dma *d = &v->audio.dma;
    memset(d, 0, sizeof(*d));
    for (unsigned i = 0; i < 8; i++) d->ch[i].masked = true;
}

static void dma_master_clear(struct dosvm_dma *d, unsigned c)
{
    /* Command, status, request, temporary and flip-flop cleared; mask set. */
    d->command[c] = d->status[c] = d->request[c] = d->temporary[c] = d->flipflop[c] = 0;
    for (unsigned i = 0; i < 4; i++) d->ch[c * 4 + i].masked = true;
}

static uint8_t dma_control(struct dosvm_dma *d, unsigned c, unsigned reg, bool write, uint8_t value)
{
    struct dosvm_dma_channel *ch = &d->ch[c * 4 + (value & 3)];
    if (!write) {
        if (reg == 8) { uint8_t s = d->status[c] | (uint8_t)(d->request[c] << 4); d->status[c] &= 0xF0; return s; }
        if (reg == 13) return d->temporary[c];
        return 0xFF; /* illegal reads (8237A figure 6) */
    }
    switch (reg) {
    case 8: d->command[c] = value; break;
    case 9: if (value & 4) d->request[c] |= 1u << (value & 3); else d->request[c] &= ~(1u << (value & 3)); break;
    case 10: ch->masked = value & 4; break;
    case 11: ch->mode = value; break;
    case 12: d->flipflop[c] = 0; break;
    case 13: dma_master_clear(d, c); break;
    case 14: for (unsigned i = 0; i < 4; i++) d->ch[c * 4 + i].masked = false; break;
    default: for (unsigned i = 0; i < 4; i++) d->ch[c * 4 + i].masked = (value >> i) & 1; break;
    }
    return 0xFF;
}

uint8_t dosvm_dma_io(struct dosvm *v, uint16_t port, bool write, uint8_t value)
{
    struct dosvm_dma *d = &v->audio.dma;
    if (port >= 0x80 && port <= 0x8F) {
        if (write) d->page[port - 0x80] = value;
        return write ? 0xFF : d->page[port - 0x80];
    }
    unsigned c, reg;
    if (port <= 0x0F) { c = 0; reg = port; }
    else if (port >= 0xC0 && port <= 0xDF && !(port & 1)) { c = 1; reg = (port - 0xC0) >> 1; }
    else return 0xFF; /* master odd ports are not decoded */
    if (reg >= 8) {
        uint8_t result = dma_control(d, c, reg, write, value);
        if (write) dosvm_sb_record_state(v, DOSVM_SB_DMA_IO, port);
        if (write && reg == 10 && !(value & 4)) {
            unsigned channel = c * 4 + (value & 3);
            const struct dosvm_dma_channel *ch = &d->ch[channel];
            uint32_t address = ((uint32_t)(d->page[dma_page_of[channel]] & (c ? 0xFE : 0xFF)) << 16) | ((uint32_t)ch->address << c);
            dosvm_audio_record(v, DOSVM_AUDIO_DMA, channel, address, ((uint32_t)ch->count + 1) << c);
        }
        return result;
    }
    struct dosvm_dma_channel *ch = &d->ch[c * 4 + (reg >> 1)];
    bool count = reg & 1, high = d->flipflop[c];
    d->flipflop[c] ^= 1;
    if (!write) {
        uint16_t x = count ? ch->count : ch->address;
        return high ? (uint8_t)(x >> 8) : (uint8_t)x;
    }
    /* Base and current registers are written together, one byte at a time. */
    uint16_t *base = count ? &ch->base_count : &ch->base_address;
    uint16_t *now = count ? &ch->count : &ch->address;
    *base = high ? (uint16_t)((*base & 0x00FF) | (value << 8)) : (uint16_t)((*base & 0xFF00) | value);
    *now = *base;
    return 0xFF;
}

static void dma_terminal(struct dosvm_dma *d, unsigned channel)
{
    struct dosvm_dma_channel *ch = &d->ch[channel];
    unsigned c = channel >> 2, bit = 1u << (channel & 3);
    d->status[c] |= bit;
    d->request[c] &= ~bit;
    if (ch->mode & 0x10) { ch->address = ch->base_address; ch->count = ch->base_count; }
    else ch->masked = true; /* count already rolled over to FFFFh */
}

/* Read-transfers (memory to device) for the virtual SB16. Returns the number
 * of transfers performed (bytes on 0-3, words on 5-7), stopping after the one
 * that reached terminal count; 0 when the channel cannot serve the request
 * (masked, controller disabled, cascade), and a negative errno on a local
 * failure: a write or illegal transfer type, or an address outside the VM. */
int dosvm_dma_read(struct dosvm *v, unsigned channel, uint8_t *buffer, unsigned transfers, bool *tc)
{
    struct dosvm_dma *d = &v->audio.dma;
    *tc = false;
    if (channel >= 8 || channel == 4) return -EINVAL;
    struct dosvm_dma_channel *ch = &d->ch[channel];
    unsigned c = channel >> 2, size = c ? 2 : 1, type = (ch->mode >> 2) & 3;
    if (ch->masked || (d->command[c] & 4) || (ch->mode >> 6) == 3) return 0;
    if (type == 1 || type == 3) { d->faults++; return -EINVAL; }
    bool down = ch->mode & 0x20;
    unsigned done = 0;
    while (done < transfers) {
        unsigned run = transfers - done, left = (unsigned)ch->count + 1;
        if (run > left) run = left;
        if (down) run = 1;
        else if (run > 0x10000u - ch->address) run = 0x10000u - ch->address; /* page wrap */
        uint32_t phys = c ? ((uint32_t)(d->page[dma_page_of[channel]] & 0xFE) << 16) | ((uint32_t)ch->address << 1)
                          : ((uint32_t)d->page[dma_page_of[channel]] << 16) | ch->address;
        if (type == 0) memset(buffer + done * size, 0xFF, run * size); /* verify: no memory cycle */
        else if (dosvm_read(v, phys, buffer + done * size, run * size)) { d->faults++; return done ? (int)done : -EFAULT; }
        ch->address = (uint16_t)(down ? ch->address - run : ch->address + run);
        done += run;
        if (run == left) { ch->count = 0xFFFF; dma_terminal(d, channel); *tc = true; break; }
        ch->count = (uint16_t)(ch->count - run);
    }
    return (int)done;
}
