/* Per-VM YMF262 (OPL3) register file, timers/status and the timestamped
 * register-event queue consumed by F4's synthesis worker. Register state only:
 * no synthesis, no physical OPL access. Status and timer bits follow the
 * YMF262/YM3812 register map (Linux snd_opl3 detection: status E0h bits,
 * 80/320 us timer resolution, low status bits 0 on OPL3); the timer, mask
 * and bank-addressing behaviour was checked against DOSBox Staging's opl.cpp
 * (behaviour only, no code). Research: docs/design/f3-acceptance.md, f3-05.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include "dosvm_internal.h"

static const uint32_t opl_clock_us[2] = {80, 320};

void dosvm_opl_init(struct dosvm *v)
{
    memset(&v->audio.opl, 0, sizeof(v->audio.opl));
    for (unsigned t = 0; t < 2; t++) v->audio.opl.timer[t].period = 256u * opl_clock_us[t];
}

/* VM microseconds: the elapsed host milliseconds plus the virtual ISA time
 * of OPL accesses inside the current millisecond (about 1 us per access,
 * capped below the next tick), so detection delay loops observe time. */
static uint64_t opl_us(const struct dosvm *v) { return v->audio.ms * 1000u + v->audio.opl.io_us; }
uint64_t dosvm_audio_now(const struct dosvm *v)
{
    return v->audio.clock + (uint64_t)v->audio.opl.io_us * DOSVM_AUDIO_PER_MS / 1000u;
}

static void opl_timer_update(struct dosvm_opl_timer *t, uint64_t now)
{
    if (!t->enabled || now < t->trigger) return;
    uint64_t late = now - t->trigger;
    if (!t->masked && !t->overflow) { t->overflow = true; t->flag_us = t->trigger; }
    t->start = now - late % t->period;
    t->trigger = t->start + t->period;
}

#define EVENTS_PER_PAGE (PAGE_SIZE / sizeof(struct dosvm_audio_event))
struct dosvm_audio_event *dosvm_event_slot(struct dosvm_audio_queue *q, unsigned index)
{
    index %= DOSVM_EVENTS;
    return &q->events[index / EVENTS_PER_PAGE][index % EVENTS_PER_PAGE];
}
#define event_slot dosvm_event_slot

void dosvm_audio_event(struct dosvm *v, uint8_t kind, uint16_t reg, uint8_t value)
{
    struct dosvm_audio_queue *q = &v->audio.q;
    q->event_writes++;
    if (!q->consumer) { q->discarded_events++; return; }
    unsigned need = q->gap ? 2 : 1;
    if (q->event_count + need > DOSVM_EVENTS) { q->lost_events++; q->gap = true; return; }
    struct dosvm_audio_event e = { dosvm_audio_now(v), v->info.key.generation, reg, value, kind };
    if (q->gap) {
        *event_slot(q, q->event_head + q->event_count++) =
            (struct dosvm_audio_event){ e.clock, e.generation, 0, 0, DOSVM_EVENT_GAP };
        q->gap = false;
    }
    *event_slot(q, q->event_head + q->event_count++) = e;
}

static void opl_write(struct dosvm *v, uint8_t value)
{
    v->audio.opl_writes++;
    struct dosvm_opl *o = &v->audio.opl;
    unsigned reg = o->address;
    uint64_t now = opl_us(v);
    o->regs[reg] = value;
    /* A0-A8 frequency writes count only while the matching B0-B8 key is on;
     * B0-B8 count only with KEYON set. Timer/detection/reset traffic cannot
     * prove music. Observe activity in each ten-second gameplay third. */
    unsigned channel=reg&255, bank=reg&256;
    if ((channel>=0xa0 && channel<=0xa8 && (o->regs[bank+channel+0x10]&0x20)) ||
        (channel>=0xb0 && channel<=0xb8 && (value&0x20))) {
        v->audio.opl_key_writes++;
        if (v->audio.title_interval) {
            uint64_t window=(g_ticks-v->audio.title_started_ms)/10000;
            if (window<3) v->audio.title_music_windows|=1u<<(unsigned)window;
        }
    }
    dosvm_audio_event(v, DOSVM_EVENT_OPL, (uint16_t)reg, value);
    if (reg == 2 || reg == 3) {
        /* A new count applies from the next reload, as on the chip. */
        o->timer[reg - 2].period = (256u - value) * opl_clock_us[reg - 2];
    } else if (reg == 4) {
        for (unsigned i = 0; i < 2; i++) opl_timer_update(&o->timer[i], now);
        if (value & 0x80) { o->timer[0].overflow = o->timer[1].overflow = false; return; } /* RST: other bits ignored */
        for (unsigned i = 0; i < 2; i++) {
            struct dosvm_opl_timer *t = &o->timer[i];
            if (value & (1u << i)) {
                if (!t->enabled) {
                    /* The prescaler runs freely: a start aligns to its clock. */
                    t->enabled = true; t->overflow = false;
                    t->start = now - now % opl_clock_us[i];
                    t->trigger = t->start + t->period;
                    t->started_us = now;
                }
            } else t->enabled = false;
            t->masked = value & (0x40u >> i);
            if (t->masked) t->overflow = false;
        }
    }
}

uint8_t dosvm_opl_status(struct dosvm *v)
{
    struct dosvm_opl *o = &v->audio.opl;
    uint64_t now = opl_us(v);
    uint8_t status = 0;
    for (unsigned i = 0; i < 2; i++) {
        opl_timer_update(&o->timer[i], now);
        if (o->timer[i].overflow) status |= 0x80 | (0x40u >> i);
    }
    return status; /* low bits 0: OPL3, not the OPL2's 06h */
}

/* port: 0/1 bank-0 address/data, 2/3 bank-1 address/data (388h-38Bh,
 * 2x0h-2x3h); 2x8h/2x9h arrive as 0/1. The chip has one address latch;
 * with NEW clear only 105h of array 1 is reachable, other array-1 addresses
 * select array 0 (OPL2 compatibility). */
uint8_t dosvm_opl_io(struct dosvm *v, unsigned port, bool write, uint8_t value)
{
    struct dosvm_opl *o = &v->audio.opl;
    if (o->io_us < 999) o->io_us++;
    if (!write) return (port & 1) ? 0xFF : dosvm_opl_status(v);
    if (port & 1) { opl_write(v, value); return 0xFF; }
    bool array1 = (port & 2) && ((o->regs[0x105] & 1) || value == 0x05);
    o->address = (uint16_t)(array1 ? 0x100u | value : value);
    return 0xFF;
}
