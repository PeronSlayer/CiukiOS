/* Per-VM Sound Blaster 16: DSP 4.05 command matrix (2.00-4.xx commands),
 * CT1745 mixer with IRQ/DMA setup and interrupt status, DMA playback on the
 * VM audio timeline and the bounded PCM segment queue F4 consumes.
 * Creative "Sound Blaster Series Hardware Programming Guide" chapters 2-4 and
 * 6 (reset, 2xEh/2xFh acknowledgement, 82h, 80h/81h, commands); DOSBox
 * Staging soundblaster.cpp for undocumented details (behaviour only, no code).
 * Block-boundary rule, IF independence and lateness: f4-audio-media-contract.md
 * "DOS VM audio path". No physical audio or DMA. Research and decisions:
 * docs/design/f3-acceptance.md, section f3-05. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/abi.h>
#include <ciuki/audio_vm.h>
#include "dosvm_internal.h"
extern void audio_vm_diagnostic(struct dosvm_key, struct audio_vm_diagnostic *) __attribute__((weak));

#define PCM_PER_PAGE (PAGE_SIZE / 4u)
#define SB_IS16(x) ((x)->format & DOSVM_FORMAT_16BIT)
static const char sb_copyright[] = "COPYRIGHT (C) CREATIVE TECHNOLOGY LTD, 1992.";

/* ---- PCM segment queue (consumer side copies under irq_save) ---- */
static int16_t *sb_pcm_slot(struct dosvm_audio_queue *q, unsigned index)
{
    index %= DOSVM_PCM_FRAMES;
    return q->pcm[index / PCM_PER_PAGE] + (index % PCM_PER_PAGE) * 2u;
}
static struct dosvm_audio_block *sb_open_segment(struct dosvm_audio_queue *q)
{ return q->open ? &q->seg[(q->seg_head + q->seg_count - 1) % DOSVM_SEGMENTS] : 0; }
static void sb_close_segment(struct dosvm *v, uint16_t flags, uint8_t cause)
{
    struct dosvm_audio_queue *q = &v->audio.q;
    struct dosvm_audio_block *b = sb_open_segment(q);
    if (!b) return;
    b->flags |= flags; b->cause = cause; b->closed = true;
    q->open = false;
}

/* Completion time of unit k (1-based) of the current advance run. */
struct sb_run { uint64_t pos; uint32_t phase, k; };
static uint64_t sb_unit_time(const struct dosvm_sb_transfer *x, const struct sb_run *r)
{
    uint64_t need = (uint64_t)r->k * x->threshold;
    need = need > r->phase ? need - r->phase : 0;
    return r->pos + (need + x->rate - 1) / x->rate;
}

static void sb_push(struct dosvm *v, int16_t left, int16_t right, const struct sb_run *r)
{
    struct dosvm_audio_queue *q = &v->audio.q;
    struct dosvm_sb16 *s = &v->audio.sb;
    q->frames++;
    if (!q->consumer) { q->discarded_frames++; return; }
    struct dosvm_audio_block *b = sb_open_segment(q);
    if (!b) {
        if (q->seg_count == DOSVM_SEGMENTS) { q->overrun_frames++; return; }
        b = &q->seg[(q->seg_head + q->seg_count++) % DOSVM_SEGMENTS];
        *b = (struct dosvm_audio_block){ .start = sb_unit_time(&s->x, r), .generation = v->info.key.generation,
            .sequence = s->sequence, .rate = s->x.rate, .format = s->x.format };
        if (s->x.kind == DOSVM_SB_SILENCE) b->flags |= DOSVM_BLOCK_SILENCE;
        if (s->x.kind == DOSVM_SB_ADPCM) b->flags |= DOSVM_BLOCK_UNSUPPORTED;
        if (s->x.late) b->flags |= DOSVM_BLOCK_LATE;
        if (s->x.stale) b->flags |= DOSVM_BLOCK_STALE;
        q->open = true;
    }
    if (q->pcm_count == DOSVM_PCM_FRAMES) { q->overrun_frames++; b->flags |= DOSVM_BLOCK_OVERRUN; return; }
    int16_t *p = sb_pcm_slot(q, q->pcm_head + q->pcm_count++);
    p[0] = left; p[1] = right;
    b->frames++;
}

static void sb_advance(struct dosvm *);

/* ---- interrupt line: 82h causes ORed onto the selected IRQ ---- */
static void sb_cause(struct dosvm *v, uint8_t cause)
{
    struct dosvm_sb16 *s = &v->audio.sb;
    bool high = s->causes & 3;
    s->causes |= cause;
    /* The 8259A latches the rising edge; a line already high adds none. */
    if (!high) { s->irq_edges++; dosvm_irq_raise(v->info.key, s->irq); }
}
static void sb_ack(struct dosvm *v, uint8_t bit)
{
    struct dosvm_sb16 *s = &v->audio.sb;
    if (s->causes & bit) {
        s->causes &= ~bit;
        s->services++; s->unserviced = 0;
        uint64_t latency = v->audio.clock - s->boundary_at;
        if (latency > s->max_service) s->max_service = (uint32_t)latency;
    }
    /* Acknowledgement of this transfer's cause ends the boundary wait; the
     * part of the next block already due on the timeline is consumed now, so
     * a responsive guest services sub-millisecond blocks without lag. */
    if (s->x.active && s->x.waiting && bit == (SB_IS16(&s->x) ? 2 : 1)) {
        s->x.waiting = false;
        sb_advance(v);
    }
}

static void sb_stop(struct dosvm *v, uint16_t flags)
{
    struct dosvm_sb16 *s = &v->audio.sb;
    sb_close_segment(v, flags, 0);
    s->x.active = s->x.waiting = s->x.highspeed = false;
    s->next_valid = false;
}

static struct dosvm_sb_transfer sb_spec(struct dosvm_sb16 *s, uint8_t kind, bool bits16, bool is_signed,
                                        bool stereo, bool auto_init, uint32_t units, uint8_t samples)
{
    struct dosvm_sb_transfer t = {0};
    uint32_t rate = s->rate_from_tc && stereo ? s->rate / 2u : s->rate;
    if (rate > DOSVM_SB_RATE_MAX) rate = DOSVM_SB_RATE_MAX;
    if (!rate) rate = 1;
    t.kind = kind; t.auto_init = auto_init; t.units = units ? units : 1; t.rate = rate;
    t.samples = samples ? samples : 1;
    t.format = (uint8_t)((bits16 ? DOSVM_FORMAT_16BIT : 0) | (is_signed ? DOSVM_FORMAT_SIGNED : 0) |
                         (stereo ? DOSVM_FORMAT_STEREO : 0));
    t.channel = bits16 ? s->dma16 : s->dma8; /* dma16 == dma8: 16-bit data on the 8-bit channel */
    /* Timeline phase per unit: a unit is one sample (two per stereo frame);
     * an ADPCM byte expands to 'samples' sample periods. */
    t.threshold = kind == DOSVM_SB_ADPCM ? DOSVM_AUDIO_HZ * t.samples : DOSVM_AUDIO_HZ / (stereo ? 2u : 1u);
    return t;
}

static void sb_begin(struct dosvm *v, const struct dosvm_sb_transfer *t)
{
    struct dosvm_sb16 *s = &v->audio.sb;
    bool highspeed = s->x.highspeed;
    s->x = *t;
    s->x.active = true; s->x.left = t->units; s->x.highspeed = highspeed;
    s->half_valid = s->carry_valid = false;
    s->sequence++;
    dosvm_audio_record(v, DOSVM_AUDIO_PLAY, t->channel, t->rate, t->units);
}
static void sb_start(struct dosvm *v, struct dosvm_sb_transfer t, bool highspeed)
{
    struct dosvm_sb16 *s = &v->audio.sb;
    /* Creative guide 3-6: programming a new mode during auto-init takes
     * effect at the end of the current block. */
    if (s->x.active && s->x.auto_init && !s->x.exit_pending && !s->x.highspeed) {
        s->next = t; s->next.highspeed = highspeed; s->next_valid = true;
        return;
    }
    if (s->x.active) { sb_stop(v, DOSVM_BLOCK_ABORTED); s->restarts++; }
    s->x.highspeed = highspeed;
    sb_begin(v, &t);
    s->pos = v->audio.clock; s->phase = 0;
}

static void sb_boundary(struct dosvm *v)
{
    struct dosvm_sb16 *s = &v->audio.sb;
    struct dosvm_sb_transfer *x = &s->x;
    uint8_t cause = SB_IS16(x) ? 2 : 1;
    s->boundaries++;
    if (x->kind == DOSVM_SB_ADPCM) s->adpcm_blocks++;
    sb_close_segment(v, DOSVM_BLOCK_COMPLETE, cause);
    s->boundary_at = v->audio.clock;
    sb_cause(v, cause);
    s->unserviced++;
    if (s->next_valid) {
        struct dosvm_sb_transfer t = s->next;
        s->next_valid = false;
        x->highspeed = t.highspeed;
        sb_begin(v, &t);
    } else if (x->auto_init && !x->exit_pending) {
        x->left = x->units; s->sequence++;
    } else {
        x->active = x->highspeed = false;
        return;
    }
    /* Generate only to the boundary, then let the guest service it: resume on
     * acknowledgement or, late, once the next block is due on the nominal
     * timeline (counted), never sooner than one host tick of real time. */
    x->waiting = true; x->late = false;
    x->stale = s->unserviced >= 2;
    if (x->stale) s->stale_blocks++;
    uint64_t due = s->pos + ((uint64_t)x->left * x->threshold + x->rate - 1) / x->rate;
    uint64_t tick = v->audio.clock + DOSVM_AUDIO_PER_MS;
    s->wait_until = due > tick ? due : tick;
}

static int16_t sb_decode(const struct dosvm_sb_transfer *x, const uint8_t *p)
{
    if (SB_IS16(x)) {
        uint16_t w = (uint16_t)(p[0] | (p[1] << 8));
        return (int16_t)((x->format & DOSVM_FORMAT_SIGNED) ? w : (uint16_t)(w ^ 0x8000u));
    }
    return (int16_t)((x->format & DOSVM_FORMAT_SIGNED) ? (int8_t)p[0] * 256 : ((int)p[0] - 128) * 256);
}
static void sb_unit(struct dosvm *v, int16_t sample, struct sb_run *r)
{
    struct dosvm_sb16 *s = &v->audio.sb;
    r->k++;
    if (!(s->x.format & DOSVM_FORMAT_STEREO)) { sb_push(v, sample, sample, r); return; }
    if (!s->half_valid) { s->half = sample; s->half_valid = true; return; }
    s->half_valid = false;
    sb_push(v, s->half, sample, r);
}

/* Consume n units at their nominal times. Returns the units consumed (fewer
 * when the DMA channel stops serving) or a negative local DMA failure. */
static int sb_consume(struct dosvm *v, unsigned n, struct sb_run *r)
{
    struct dosvm_sb16 *s = &v->audio.sb;
    struct dosvm_sb_transfer *x = &s->x;
    if (x->kind == DOSVM_SB_SILENCE) {
        for (unsigned i = 0; i < n; i++) sb_unit(v, 0, r);
        s->units += n;
        return (int)n;
    }
    uint8_t buffer[257];
    bool word_channel = x->channel >= 4;
    unsigned unit_bytes = SB_IS16(x) ? 2 : 1, done = 0;
    while (done < n) {
        unsigned want = n - done;
        if (want > 128) want = 128;
        unsigned carry = s->carry_valid ? 1 : 0;
        if (carry) buffer[0] = s->carry;
        unsigned transfers = word_channel ? want : want * unit_bytes - carry;
        bool tc;
        int got = dosvm_dma_read(v, x->channel, buffer + carry, transfers, &tc);
        if (got < 0) return got;
        if (!got) break;
        unsigned bytes = carry + (unsigned)got * (word_channel ? 2u : 1u);
        unsigned units = bytes / unit_bytes;
        s->carry_valid = bytes % unit_bytes;
        if (s->carry_valid) s->carry = buffer[bytes - 1];
        for (unsigned i = 0; i < units; i++) {
            if (x->kind == DOSVM_SB_ADPCM) for (unsigned j = 0; j < x->samples; j++) sb_unit(v, 0, r);
            else sb_unit(v, sb_decode(x, buffer + i * unit_bytes), r);
        }
        done += units;
        s->units += units;
    }
    return (int)done;
}

/* DSP playback on the VM timeline, driven by dosvm_tick whatever the guest
 * IF/PIC state. Each call processes at most DOSVM_AUDIO_SLICE frames of DSP
 * time and never crosses a block boundary without a service opportunity. */
static void sb_advance(struct dosvm *v)
{
    struct dosvm_sb16 *s = &v->audio.sb;
    uint64_t now = v->audio.clock;
    uint32_t budget = DOSVM_AUDIO_SLICE;
    for (unsigned guard = 0; guard < 32 && s->x.active && budget; guard++) {
        struct dosvm_sb_transfer *x = &s->x;
        if (x->waiting) {
            if (now < s->wait_until) return;
            x->waiting = false; x->late = true; s->late++;
        }
        if (x->paused) { s->pos = now; return; }
        if (now <= s->pos) return;
        uint64_t span = now - s->pos;
        if (span > DOSVM_AUDIO_BACKLOG) {
            s->dropped_frames += span - DOSVM_AUDIO_BACKLOG;
            s->pos = now - DOSVM_AUDIO_BACKLOG; span = DOSVM_AUDIO_BACKLOG;
        }
        if (span > s->max_lag) s->max_lag = (uint32_t)span;
        if (span > budget) span = budget;
        uint64_t due = ((uint64_t)s->phase + span * x->rate) / x->threshold;
        bool boundary = due >= x->left;
        unsigned n = boundary ? x->left : (unsigned)due;
        uint64_t need = (uint64_t)x->left * x->threshold;
        need = need > s->phase ? need - s->phase : 0;
        uint64_t frames = boundary ? (need + x->rate - 1) / x->rate : span;
        struct sb_run run = { s->pos, s->phase, 0 };
        int got = n ? sb_consume(v, n, &run) : 0;
        if (got < 0) {
            /* Address outside this VM or a non-read transfer type: the
             * transfer fails locally; no IRQ, the DSP stays usable. */
            s->last_error = got == -EINVAL ? DOSVM_AUDIO_DMA_MODE : DOSVM_AUDIO_DMA_FAULT;
            sb_stop(v, DOSVM_BLOCK_ABORTED);
            return;
        }
        x->left -= (unsigned)got;
        if ((unsigned)got < n) {
            /* DREQ unanswered (masked/disabled channel): the DSP waits and
             * the elapsed time is lost, as on the card. */
            sb_close_segment(v, 0, 0);
            s->dma_wait_frames += (uint32_t)span;
            s->pos = now;
            return;
        }
        s->pos += frames;
        s->phase = (uint32_t)(s->phase + frames * x->rate - (uint64_t)n * x->threshold);
        budget -= (uint32_t)frames;
        if (boundary) sb_boundary(v);
    }
}

/* ---- CT1745 mixer ---- */
static void sb_mixer_reset(struct dosvm_sb16 *s)
{
    memset(s->mixer, 0, sizeof(s->mixer));
    for (unsigned i = 0x30; i <= 0x35; i++) s->mixer[i] = 24u << 3; /* -14 dB */
    s->mixer[0x3C] = 0x1F; s->mixer[0x3D] = 0x15; s->mixer[0x3E] = 0x0B;
    for (unsigned i = 0x44; i <= 0x47; i++) s->mixer[i] = 0x80;     /* 0 dB tone */
}
static uint8_t sb_irq_bits(unsigned irq) { return irq == 2 ? 1 : irq == 5 ? 2 : irq == 7 ? 4 : irq == 10 ? 8 : 0; }
static uint8_t sb_mixer_read(struct dosvm_sb16 *s)
{
    uint8_t i = s->mixer_index, *m = s->mixer;
    switch (i) {
    case 0x04: return (uint8_t)(((m[0x32] >> 4) << 4) | (m[0x33] >> 4));
    case 0x0A: return m[0x3A] >> 5;
    case 0x22: return (uint8_t)(((m[0x30] >> 4) << 4) | (m[0x31] >> 4));
    case 0x26: return (uint8_t)(((m[0x34] >> 4) << 4) | (m[0x35] >> 4));
    case 0x28: return (uint8_t)(((m[0x36] >> 4) << 4) | (m[0x37] >> 4));
    case 0x2E: return (uint8_t)(((m[0x38] >> 4) << 4) | (m[0x39] >> 4));
    case 0x80: return sb_irq_bits(s->irq);
    case 0x81: return (uint8_t)((1u << s->dma8) | (s->dma16 >= 5 ? 1u << s->dma16 : 0));
    case 0x82: return (uint8_t)(s->causes | 0x20); /* SB16 reports 20h in the reserved bits */
    default: return m[i];
    }
}
static void sb_compat(uint8_t *m, unsigned left, uint8_t value)
{
    /* CT1345 4-bit level n maps onto the CT1745 5-bit level 2n+1. */
    m[left] = (uint8_t)((((value >> 4) << 1) | 1) << 3);
    m[left + 1] = (uint8_t)((((value & 15) << 1) | 1) << 3);
}
static void sb_mixer_write(struct dosvm_sb16 *s, uint8_t value)
{
    uint8_t i = s->mixer_index, *m = s->mixer;
    switch (i) {
    case 0x00: sb_mixer_reset(s); return;
    case 0x04: sb_compat(m, 0x32, value); return;
    case 0x0A: m[0x3A] = (uint8_t)((((value & 7) << 2) | 1) << 3); return;
    case 0x22: sb_compat(m, 0x30, value); return;
    case 0x26: sb_compat(m, 0x34, value); return;
    case 0x28: sb_compat(m, 0x36, value); return;
    case 0x2E: sb_compat(m, 0x38, value); return;
    case 0x80:
        if (value == 1 || value == 2 || value == 4 || value == 8) s->irq = value == 1 ? 2 : value == 2 ? 5 : value == 4 ? 7 : 10;
        else s->unsupported++; /* exactly one IRQ bit (guide 2-6) */
        return;
    case 0x81: {
        uint8_t low = value & 0x0B, high = value & 0xE0;
        if ((low != 1 && low != 2 && low != 8) || (high & (high - 1))) { s->unsupported++; return; }
        s->dma8 = low == 1 ? 0 : low == 2 ? 1 : 3;
        s->dma16 = high == 0x20 ? 5 : high == 0x40 ? 6 : high == 0x80 ? 7 : s->dma8;
        return;
    }
    case 0x82: return; /* read-only interrupt status */
    default: break;
    }
    if (i >= 0x30 && i <= 0x3A) value &= 0xF8;
    else if (i == 0x3B || (i >= 0x3F && i <= 0x42)) value &= 0xC0;
    else if (i == 0x3C) value &= 0x1F;
    else if (i == 0x3D || i == 0x3E) value &= 0x7F;
    else if (i == 0x43) value &= 0x01;
    else if (i >= 0x44 && i <= 0x47) value &= 0xF0;
    m[i] = value;
}

/* ---- DSP ---- */
static void sb_reply(struct dosvm_sb16 *s, uint8_t value)
{
    if (s->out_count == sizeof(s->out)) return;
    s->out[(s->out_head + s->out_count++) % sizeof(s->out)] = value;
}
static unsigned sb_args(uint8_t c)
{
    if (c >= 0xB0 && c <= 0xCF) return 3;
    switch (c) {
    case 0x04: case 0x08: case 0x0F: case 0x10: case 0x38: case 0x40:
    case 0xE0: case 0xE2: case 0xE4: case 0xF9: return 1;
    case 0x05: case 0x0E: case 0x14: case 0x15: case 0x16: case 0x17: case 0x24:
    case 0x41: case 0x42: case 0x48: case 0x74: case 0x75: case 0x76: case 0x77: case 0x80: return 2;
    default: return 0;
    }
}
static void sb_set_pause(struct dosvm *v, bool bits16, bool paused)
{
    struct dosvm_sb16 *s = &v->audio.sb;
    if (!s->x.active || !SB_IS16(&s->x) != !bits16) return;
    if (paused && !s->x.paused) sb_close_segment(v, 0, 0); /* time gap: new segment on resume */
    s->x.paused = paused;
    if (!paused) s->pos = v->audio.clock;
}
static void sb_execute(struct dosvm *v)
{
    struct dosvm_sb16 *s = &v->audio.sb;
    uint8_t c = s->command, *a = s->in;
    uint32_t length = (uint32_t)a[0] | ((uint32_t)a[1] << 8);
    bool legacy_stereo = s->mixer[0x0E] & 2; /* SB Pro output stereo bit (8-bit legacy commands) */
    if (c >= 0xB0 && c <= 0xCF) {
        if (c & 8) { s->unsupported++; return; } /* A/D: recording is not advertised */
        sb_start(v, sb_spec(s, DOSVM_SB_PCM, c < 0xC0, a[0] & 0x10, a[0] & 0x20, c & 4,
                            ((uint32_t)a[1] | ((uint32_t)a[2] << 8)) + 1, 1), false);
        return;
    }
    switch (c) {
    case 0x10: s->direct_dac++; dosvm_audio_event(v, DOSVM_EVENT_DAC, 0, a[0]); break;
    case 0x14: sb_start(v, sb_spec(s, DOSVM_SB_PCM, false, false, legacy_stereo, false, length + 1, 1), false); break;
    case 0x91: sb_start(v, sb_spec(s, DOSVM_SB_PCM, false, false, legacy_stereo, false, s->block_units, 1), true); break;
    case 0x1C: sb_start(v, sb_spec(s, DOSVM_SB_PCM, false, false, legacy_stereo, true, s->block_units, 1), false); break;
    case 0x90: sb_start(v, sb_spec(s, DOSVM_SB_PCM, false, false, legacy_stereo, true, s->block_units, 1), true); break;
    /* Creative ADPCM: DMA, block timing and IRQs are kept; PCM is silence and
     * flagged unsupported (no decoder is advertised in F3). */
    case 0x16: case 0x17: sb_start(v, sb_spec(s, DOSVM_SB_ADPCM, false, false, false, false, length + 1, 4), false); break;
    case 0x74: case 0x75: sb_start(v, sb_spec(s, DOSVM_SB_ADPCM, false, false, false, false, length + 1, 2), false); break;
    case 0x76: case 0x77: sb_start(v, sb_spec(s, DOSVM_SB_ADPCM, false, false, false, false, length + 1, 3), false); break;
    case 0x1F: sb_start(v, sb_spec(s, DOSVM_SB_ADPCM, false, false, false, true, s->block_units, 4), false); break;
    case 0x7D: sb_start(v, sb_spec(s, DOSVM_SB_ADPCM, false, false, false, true, s->block_units, 2), false); break;
    case 0x7F: sb_start(v, sb_spec(s, DOSVM_SB_ADPCM, false, false, false, true, s->block_units, 3), false); break;
    case 0x80: sb_start(v, sb_spec(s, DOSVM_SB_SILENCE, false, false, false, false, length + 1, 1), false); break;
    case 0x20: sb_reply(s, 0x80); s->unsupported++; break; /* direct ADC: midpoint, recording not advertised */
    case 0x34: case 0x35: case 0x36: case 0x37: s->uart = true; break;
    case 0x38: s->midi_bytes++; break;
    case 0x40: s->rate = 1000000u / (256u - a[0]); s->rate_from_tc = true; break;
    case 0x41: case 0x42: {
        uint32_t rate = ((uint32_t)a[0] << 8) | a[1];
        s->rate = rate < DOSVM_SB_RATE_MIN ? DOSVM_SB_RATE_MIN : rate > DOSVM_SB_RATE_MAX ? DOSVM_SB_RATE_MAX : rate;
        s->rate_from_tc = false;
        break;
    }
    case 0x45: case 0x47: /* continue auto-init: cancels a pending exit (DSP 4.xx) */
        if (s->x.active && s->x.auto_init && !SB_IS16(&s->x) == (c == 0x45)) s->x.exit_pending = false;
        break;
    case 0x48: s->block_units = length + 1; break;
    case 0xA0: case 0xA8: break; /* input stereo mode: no recording path */
    case 0xD0: sb_set_pause(v, false, true); break;
    case 0xD4: sb_set_pause(v, false, false); break;
    case 0xD5: sb_set_pause(v, true, true); break;
    case 0xD6: sb_set_pause(v, true, false); break;
    case 0xD1: s->speaker = true; break;  /* 4.xx: status latch only, never gates the DAC */
    case 0xD3: s->speaker = false; break;
    case 0xD8: sb_reply(s, s->speaker ? 0xFF : 0x00); break;
    case 0xD9: case 0xDA:
        if (s->x.active && s->x.auto_init && !SB_IS16(&s->x) == (c == 0xDA)) s->x.exit_pending = true;
        break;
    case 0xE0: sb_reply(s, (uint8_t)~a[0]); break;
    case 0xE1:
        sb_reply(s, 4); sb_reply(s, 5);
        dosvm_audio_record(v, DOSVM_AUDIO_VERSION, 4, 5, 0); break;
    case 0xE3: for (unsigned i = 0; i < sizeof(sb_copyright); i++) sb_reply(s, (uint8_t)sb_copyright[i]); break;
    case 0xE4: s->test = a[0]; break;
    case 0xE8: sb_reply(s, s->test); break;
    case 0xF2: s->forced++; sb_cause(v, 1); break;
    case 0xF3: s->forced++; sb_cause(v, 2); break;
    case 0x0F: sb_reply(s, 0); s->unsupported++; break; /* ASP register read: no ASP */
    default: s->unsupported++; break; /* DMA ADC, MIDI input, ASP, E2h and undocumented */
    }
}
static void sb_dsp_write(struct dosvm *v, uint8_t value)
{
    struct dosvm_sb16 *s = &v->audio.sb;
    if (s->reset_high) return;
    if (s->uart) { s->midi_bytes++; return; }        /* MIDI UART: data until reset */
    if (s->x.active && s->x.highspeed) { s->unsupported++; return; } /* no commands in high-speed mode */
    if (!s->in_need) {
        s->command = value; s->in_count = 0; s->in_need = (uint8_t)sb_args(value);
        if (!s->in_need) sb_execute(v);
        return;
    }
    s->in[s->in_count++] = value;
    if (s->in_count == s->in_need) { s->in_need = 0; sb_execute(v); }
}
static void sb_reset_port(struct dosvm *v, uint8_t value)
{
    struct dosvm_sb16 *s = &v->audio.sb;
    if (value & 1) { s->reset_high = true; return; }
    if (!s->reset_high) return;
    s->reset_high = false;
    /* High-speed and UART resets restore the earlier parameters. */
    bool restore = s->uart || (s->x.active && s->x.highspeed);
    sb_stop(v, DOSVM_BLOCK_ABORTED);
    s->uart = false;
    s->causes = 0; s->unserviced = 0;
    s->in_need = s->in_count = 0; s->out_count = 0;
    if (!restore) { s->rate = 22050; s->rate_from_tc = false; s->speaker = false; }
    s->resets++;
    dosvm_audio_record(v, DOSVM_AUDIO_RESET, s->resets, 0xAA, 0);
    sb_reply(s, 0xAA);
}
static uint8_t sb_io(struct dosvm *v, unsigned offset, bool write, uint8_t value)
{
    struct dosvm_sb16 *s = &v->audio.sb;
    if (offset <= 3) return dosvm_opl_io(v, offset, write, value);
    if (offset == 8 || offset == 9) return dosvm_opl_io(v, offset - 8, write, value);
    if (write) {
        if (offset == 4) s->mixer_index = value;
        else if (offset == 5) sb_mixer_write(s, value);
        else if (offset == 6) sb_reset_port(v, value);
        else if (offset == 0xC) sb_dsp_write(v, value);
        return 0xFF;
    }
    switch (offset) {
    case 4: return s->mixer_index;
    case 5: return sb_mixer_read(s);
    case 0xA:
        if (s->out_count) { s->last = s->out[s->out_head]; s->out_head = (uint8_t)((s->out_head + 1) % sizeof(s->out)); s->out_count--; }
        return s->last;
    case 0xC: return s->reset_high ? 0xFF : 0x7F;     /* bit 7 clear: ready for a byte */
    case 0xE: sb_ack(v, 1); return s->out_count ? 0xFF : 0x7F; /* data available; 8-bit acknowledge */
    case 0xF: sb_ack(v, 2); return 0xFF;               /* 16-bit acknowledge */
    default: return 0xFF;
    }
}

/* ---- ports, lifecycle and timeline ---- */
static uint8_t audio_byte(struct dosvm *v, uint16_t port, bool write, uint8_t value)
{
    uint16_t base = v->audio.sb.base;
    if (port <= 0x0F || (port >= 0x80 && port <= 0x8F) || (port >= 0xC0 && port <= 0xDF))
        return dosvm_dma_io(v, port, write, value);
    if (port >= DOSVM_OPL_BASE && port <= DOSVM_OPL_BASE + 3) return dosvm_opl_io(v, port - DOSVM_OPL_BASE, write, value);
    if (port >= base && port <= base + 0xF) return sb_io(v, port - base, write, value);
    return 0xFF;
}
/* Byte devices on an 8-bit ISA bus: wider CPU accesses are split into
 * consecutive byte cycles (the mixer index/data word idiom relies on it). */
int dosvm_audio_port(struct dosvm *v, uint16_t port, unsigned width, bool write, uint32_t *value, void *arg)
{
    (void)arg;
    if (width != 1 && width != 2 && width != 4) return -V86_EPERM;
    uint32_t result = 0;
    for (unsigned i = 0; i < width; i++)
        result |= (uint32_t)audio_byte(v, (uint16_t)(port + i), write, (uint8_t)(*value >> (8 * i))) << (8 * i);
    if (!write) *value = result;
    return 0;
}

int dosvm_audio_init(struct dosvm *v)
{
    struct dosvm_audio *a = &v->audio;
    memset(a, 0, sizeof(*a));
    a->diag.last_summary = g_ticks; a->diag.next_summary = g_ticks + 2000;
    for (unsigned i = 0; i < DOSVM_AUDIO_PAGES; i++) {
        uint32_t phys = pmm_alloc_zone(ZONE_NORMAL);
        if (!phys) return -ENOMEM;
        memset(P2V(phys), 0, PAGE_SIZE);
        a->q.pages[a->q.page_count++] = phys;
        v->info.resident += PAGE_SIZE;
    }
    for (unsigned i = 0; i < DOSVM_PCM_PAGES; i++) a->q.pcm[i] = P2V(a->q.pages[i]);
    for (unsigned i = 0; i < DOSVM_EVENT_PAGES; i++) a->q.events[i] = P2V(a->q.pages[DOSVM_PCM_PAGES + i]);
    dosvm_dma_init(v);
    dosvm_opl_init(v);
    struct dosvm_sb16 *s = &a->sb;
    s->base = DOSVM_SB_BASE; s->irq = DOSVM_SB_IRQ; s->dma8 = DOSVM_SB_DMA8; s->dma16 = DOSVM_SB_DMA16;
    s->rate = 22050; s->block_units = 0x800;
    sb_mixer_reset(s);
    static const uint16_t ranges[][2] = {{0x00, 0x0F}, {0x80, 0x8F}, {0xC0, 0xDF},
        {DOSVM_SB_BASE, DOSVM_SB_BASE + 0xF}, {DOSVM_OPL_BASE, DOSVM_OPL_BASE + 3}};
    for (unsigned i = 0; i < ARRAY_SIZE(ranges); i++) {
        int e = dosvm_port_register(v->info.key, ranges[i][0], ranges[i][1], dosvm_audio_port, 0);
        if (e) return e;
    }
    return 0;
}
void dosvm_audio_close(struct dosvm *v)
{
    struct dosvm_audio_queue *q = &v->audio.q;
    q->consumer = 0;
    for (unsigned i = 0; i < q->page_count; i++) pmm_free(q->pages[i]);
    q->page_count = 0;
    memset(q->pcm, 0, sizeof(q->pcm)); memset(q->events, 0, sizeof(q->events));
}
void dosvm_audio_advance(struct dosvm *v, uint64_t elapsed)
{
    if (!v->audio.q.page_count) return;
    v->audio.ms += elapsed;
    v->audio.clock += elapsed * DOSVM_AUDIO_PER_MS;
    v->audio.opl.io_us = 0;
    sb_advance(v);
}

/* ---- F4 consumer interface (ciuki/dosvm_audio.h) ---- */
static void audio_queue_reset(struct dosvm_audio_queue *q)
{
    q->pcm_head = q->pcm_count = q->event_head = q->event_count = q->seg_head = q->seg_count = 0;
    q->open = q->gap = false;
}
static struct dosvm *audio_vm(struct dosvm_key key, uint32_t consumer)
{
    struct dosvm *v = dosvm_lookup(key);
    if (!v || !dosvm_valid(key) || !v->audio.q.page_count) return 0;
    if (consumer != UINT32_MAX && (!consumer || v->audio.q.consumer != consumer)) return 0;
    return v;
}
int dosvm_audio_attach(struct dosvm_key key, uint32_t *consumer)
{
    struct dosvm *v = audio_vm(key, UINT32_MAX);
    if (!v || !consumer) return -EINVAL;
    uint32_t token = gen_alloc();
    if (!token) return -ENOSPC;
    uint32_t flags = irq_save();
    audio_queue_reset(&v->audio.q);
    v->audio.q.consumer = token;
    irq_restore(flags);
    *consumer = token;
    return 0;
}
int dosvm_audio_detach(struct dosvm_key key, uint32_t consumer)
{
    struct dosvm *v = audio_vm(key, consumer);
    if (!v) return -EINVAL;
    uint32_t flags = irq_save();
    audio_queue_reset(&v->audio.q);
    v->audio.q.consumer = 0;
    irq_restore(flags);
    return 0;
}
int dosvm_audio_fetch(struct dosvm_key key, uint32_t consumer, struct dosvm_audio_block *block,
                      int16_t *stereo, unsigned max)
{
    struct dosvm *v = audio_vm(key, consumer);
    if (!v || !block || (max && !stereo)) return -EINVAL;
    struct dosvm_audio_queue *q = &v->audio.q;
    uint32_t flags = irq_save();
    if (!q->seg_count) { irq_restore(flags); return -EAGAIN; }
    struct dosvm_audio_block *b = &q->seg[q->seg_head];
    unsigned n = b->frames - b->offset;
    if (n > max) n = max;
    *block = *b;
    for (unsigned i = 0; i < n; i++) {
        const int16_t *p = sb_pcm_slot(q, q->pcm_head + i);
        stereo[i * 2] = p[0]; stereo[i * 2 + 1] = p[1];
    }
    q->pcm_head = (q->pcm_head + n) % DOSVM_PCM_FRAMES; q->pcm_count -= n;
    b->offset += n;
    /* A closed segment is retired once its frames and its final state
     * (COMPLETE, cause, flags) have been returned. */
    if (b->closed && b->offset == b->frames) {
        block->closed = true;
        q->seg_head = (q->seg_head + 1) % DOSVM_SEGMENTS; q->seg_count--;
    } else block->closed = false;
    irq_restore(flags);
    return (int)n;
}
int dosvm_audio_events(struct dosvm_key key, uint32_t consumer, struct dosvm_audio_event *out, unsigned max)
{
    struct dosvm *v = audio_vm(key, consumer);
    if (!v || (max && !out)) return -EINVAL;
    struct dosvm_audio_queue *q = &v->audio.q;
    uint32_t flags = irq_save();
    unsigned n = q->event_count < max ? q->event_count : max;
    for (unsigned i = 0; i < n; i++) out[i] = *dosvm_event_slot(q, q->event_head + i);
    q->event_head = (q->event_head + n) % DOSVM_EVENTS; q->event_count -= n;
    irq_restore(flags);
    return (int)n;
}
bool dosvm_audio_info(struct dosvm_key key, struct dosvm_audio_info *out)
{
    struct dosvm *v = dosvm_lookup(key);
    if (!out || !v || v->info.state == DOSVM_DEAD) return false;
    uint32_t flags = irq_save();
    const struct dosvm_sb16 *s = &v->audio.sb;
    const struct dosvm_audio_queue *q = &v->audio.q;
    const struct dosvm_opl *o = &v->audio.opl;
    memset(out, 0, sizeof(*out));
    out->clock = v->audio.clock; out->dsp_clock = s->pos;
    out->frames = q->frames; out->discarded_frames = q->discarded_frames;
    out->overrun_frames = q->overrun_frames; out->dropped_frames = s->dropped_frames;
    out->events = q->event_writes; out->discarded_events = q->discarded_events; out->lost_events = q->lost_events;
    out->base = s->base; out->irq = s->irq; out->dma8 = s->dma8; out->dma16 = s->dma16;
    out->boundaries = s->boundaries; out->irq_edges = s->irq_edges; out->services = s->services;
    out->late = s->late; out->stale_blocks = s->stale_blocks; out->forced = s->forced;
    out->units = s->units; out->dma_faults = v->audio.dma.faults; out->dma_wait_frames = s->dma_wait_frames;
    out->unsupported = s->unsupported; out->adpcm_blocks = s->adpcm_blocks; out->restarts = s->restarts;
    out->resets = s->resets; out->max_service = s->max_service; out->max_lag = s->max_lag;
    out->direct_dac = s->direct_dac; out->midi_bytes = s->midi_bytes;
    out->pcm_queued = q->pcm_count; out->blocks_queued = q->seg_count; out->events_queued = q->event_count;
    out->causes = s->causes; out->active = s->x.active; out->paused = s->x.paused; out->waiting = s->x.waiting;
    out->auto_init = s->x.auto_init; out->bits = SB_IS16(&s->x) ? 16 : 8;
    out->stereo = !!(s->x.format & DOSVM_FORMAT_STEREO); out->speaker = s->speaker;
    out->highspeed = s->x.highspeed; out->last_error = s->last_error;
    for (unsigned i = 0; i < 2; i++) {
        out->master[i] = s->mixer[0x30 + i] >> 3; out->voice[i] = s->mixer[0x32 + i] >> 3;
        out->fm[i] = s->mixer[0x34 + i] >> 3;
        out->output_gain[i] = s->mixer[0x41 + i] >> 6;
        out->opl_period_us[i] = o->timer[i].period;
        out->opl_start_us[i] = o->timer[i].started_us; out->opl_flag_us[i] = o->timer[i].flag_us;
    }
    out->opl_status = dosvm_opl_status(v);
    out->opl_new = o->regs[0x105] & 1; out->opl_pairs = o->regs[0x104] & 0x3F;
    out->consumer = q->consumer != 0;
    irq_restore(flags);
    return true;
}
bool dosvm_audio_opl_registers(struct dosvm_key key, uint8_t regs[512], uint64_t *clock)
{
    struct dosvm *v = dosvm_lookup(key);
    if (!regs || !v || v->info.state == DOSVM_DEAD) return false;
    uint32_t flags = irq_save();
    memcpy(regs, v->audio.opl.regs, 512);
    if (clock) *clock = dosvm_audio_now(v);
    irq_restore(flags);
    return true;
}

/* f4-13: recording does no formatting or output and is safe in the device
 * timer/guest IRQ. Discovery has a lifetime budget; DMA blocks cannot turn
 * verbose mode into a print on every guest interrupt. */
void dosvm_audio_record(struct dosvm *v, unsigned kind, uint32_t a, uint32_t b, uint32_t c)
{
    if (!(g_boot.flags & CBI_F_VERBOSE) || !kind || kind >= ARRAY_SIZE(v->audio.diag.used)) return;
    uint32_t f = irq_save();
    if (v->audio.diag.used[kind] < 4) {
        v->audio.diag.used[kind]++;
        if (v->audio.diag.count == ARRAY_SIZE(v->audio.diag.event)) v->audio.diag.dropped++;
        else {
            unsigned at = (v->audio.diag.head + v->audio.diag.count++) % ARRAY_SIZE(v->audio.diag.event);
            v->audio.diag.event[at].kind = (uint8_t)kind;
            v->audio.diag.event[at].a = a; v->audio.diag.event[at].b = b; v->audio.diag.event[at].c = c;
        }
    }
    irq_restore(f);
}
/* The CiukiDOS shared environment is an MCB at 1630h (runtime kernel,
 * DOS_ENV_SEG), also inherited by Wolf3D. A DPMI client may have another
 * environment: inspect the actual segment recorded at its entry instead.
 * This reports what is offered to getenv, not an assertion that the game
 * called getenv. Only BLASTER is printed, never other environment data. */
static void audio_blaster(struct dosvm *v, char out[40])
{
    uint16_t env = v->dpmi.active && v->dpmi.clients ? v->dpmi.client[v->dpmi.clients - 1].env : 0x1630;
    char bytes[512];
    memcpy(out, "missing", sizeof("missing"));
    if (!env || dosvm_read(v, (uint32_t)env << 4, bytes, sizeof(bytes))) return;
    for (unsigned at = 0; at < sizeof(bytes) && bytes[at];) {
        unsigned end = at;
        while (end < sizeof(bytes) && bytes[end]) end++;
        if (end == sizeof(bytes)) return;
        if (end - at >= 8 && !memcmp(bytes + at, "BLASTER=", 8)) {
            unsigned n = end - at - 8;
            if (n >= 40) { memcpy(out, "too_long", sizeof("too_long")); return; }
            for (unsigned i = 0; i < n; i++) {
                unsigned char b = (unsigned char)bytes[at + 8 + i];
                out[i] = b == ' ' ? '_' : b >= 33 && b <= 126 ? (char)b : '?';
            }
            out[n] = 0; return;
        }
        at = end + 1;
    }
}
void dosvm_audio_log(struct dosvm *v)
{
    if (!(g_boot.flags & CBI_F_VERBOSE) || v->info.state >= DOSVM_STOPPING) return;
    for (unsigned i = 0; i < v->dpmi.depth; i++)
        if (v->dpmi.rec[i].flags & DPMI_RF_HWINT) return;
    /* One short line per 100 ms, physical IF enabled, outside the complete
     * guest IRQ chain (the caller owns that check). Never called by tick or
     * the refill worker. UART polling must not consume a whole DMA lead. */
    uint32_t f = irq_save();
    if (!(f & V86_IF)) { irq_restore(f); return; }
    uint64_t now = g_ticks;
    if (now < v->audio.diag.next_log) { irq_restore(f); return; }
    v->audio.diag.next_log = now + 100;
    unsigned kind = 0; uint32_t a = 0, b = 0, c = 0;
    if (v->audio.diag.attachment && v->audio.diag.count) {
        unsigned at = v->audio.diag.head;
        kind = v->audio.diag.event[at].kind; a = v->audio.diag.event[at].a;
        b = v->audio.diag.event[at].b; c = v->audio.diag.event[at].c;
        v->audio.diag.head = (uint8_t)((at + 1) % ARRAY_SIZE(v->audio.diag.event));
        v->audio.diag.count--;
    }
    irq_restore(f);
    unsigned id = v->info.key.id;
    if (!v->audio.diag.attachment) {
        v->audio.diag.attachment = true;
        klog("[vm-audio] vm=%u gen=%u attach=%d base=220 irq=7 dma=1,5 dsp=4.05", id, v->info.key.generation, v->audio.diag.attach_error);
    } else if (kind == DOSVM_AUDIO_RESET) klog("[vm-audio] vm=%u reset=%u reply=%02x", id, a, b);
    else if (kind == DOSVM_AUDIO_VERSION) klog("[vm-audio] vm=%u version=%u.%02u", id, a, b);
    else if (kind == DOSVM_AUDIO_PLAY) klog("[vm-audio] vm=%u play_dma=%u hz=%u units=%u", id, a, b, c);
    else if (kind == DOSVM_AUDIO_DMA) klog("[vm-audio] vm=%u dma=%u address=%06x bytes=%u", id, a, b, c);
    else if (!v->audio.diag.environment && v->audio.sb.resets) {
        char blaster[40]; audio_blaster(v, blaster); v->audio.diag.environment = true;
        klog("[vm-audio] vm=%u blaster=%s source=guest_env", id, blaster);
    } else if (v->audio.sb.resets || v->audio.q.event_writes) {
        struct audio_vm_diagnostic d = { .error=-ENODEV };
        if (audio_vm_diagnostic) audio_vm_diagnostic(v->info.key, &d);
        if (v->audio.diag.summary_pending) {
            v->audio.diag.summary_pending = false;
            klog("[vm-audio] vm=%u pcm=%llu fm=%llu fifo=%llu halt=%llu drop=%u", id,
                 (unsigned long long)d.pcm, (unsigned long long)d.opl,
                 (unsigned long long)d.fifo, (unsigned long long)d.halts, v->audio.diag.dropped);
        } else if (now >= v->audio.diag.next_summary) {
            uint64_t elapsed = now - v->audio.diag.last_summary;
            uint64_t buffers = elapsed && d.published >= v->audio.diag.published ?
                (d.published - v->audio.diag.published) * 1000 / elapsed : 0;
            v->audio.diag.last_summary = now; v->audio.diag.published = d.published;
            v->audio.diag.next_summary = now + 2000; v->audio.diag.summary_pending = true;
            klog("[vm-audio] vm=%u ms=%llu ready=%u err=%d buffers_ps=%llu rate=%u starved=%llu attached=%u", id,
                 (unsigned long long)now, d.ready, d.error, (unsigned long long)buffers,
                 d.rate, (unsigned long long)d.starved, d.attached);
        }
    }
}
