/*
 *  Copyright (C) 2002-2021  The DOSBox Team
 *  C port for CiukiOS (f4-05), 2026.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/*
 * DOSBox implementation of a combined Yamaha YMF262 and Yamaha YM3812
 * emulator (DBOPL), ported from C++ to C17 for the CiukiOS kernel mixer.
 * Source: third_party/vsbhda/source-75fa4bb.tar.gz, src/DBOPL.CPP and
 * DBOPL.H (DOSBox Team, GPL-2.0-or-later), built there with
 * DBOPL_WAVE == WAVE_TABLEMUL and without WAVE_PRECISION.
 *
 * Changes from upstream, all behaviour-preserving for the synthesis:
 *  - the floating-point tables (MulTable, the sine and exponential waves,
 *    and Chip::Setup's rate tables) are integer constants generated for
 *    48000 Hz in opl3_tables.h (tests/host/test_opl3_tables.py), so this
 *    file contains no FPU code; the integer tables are built at first use;
 *  - classes, member-function pointers and pointer-offset tables became C
 *    structures, a state switch and index tables;
 *  - samples accumulate in int32_t as in upstream DOSBox (VSBHDA's int16_t
 *    output buffer is not reproduced); the chip is always an OPL3
 *    (Chip(true)), OPL2 compatibility is its NEW-clear mode;
 *  - left shifts of signed samples are written as multiplications.
 * Host evidence: the port is compared sample for sample with the pinned
 * C++ (tests/host/opl3_ref_test.cpp). SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <ciuki/opl3.h>
#ifdef CIUKI_HOST_OPL3
#include <string.h>
#else
#include <ciuki/kernel.h>
#endif
#include "opl3_tables.h"

#define WAVE_SH 22
#define LFO_SH 12
#define LFO_MAX (256u << LFO_SH)
#define ENV_BITS 9
#define ENV_EXTRA 0
#define ENV_MIN 0
#define ENV_MAX 511
#define ENV_LIMIT ((12 * 256) >> (3 - ENV_EXTRA))
#define ENV_SILENT(x) ((x) >= ENV_LIMIT)
#define RATE_SH 24
#define RATE_MASK ((1u << RATE_SH) - 1)
#define MUL_SH 16
#define TREMOLO_TABLE 52
#define SHIFT_KSLBASE 16
#define SHIFT_KEYCODE 24
#define MASK_KSR 0x10
#define MASK_SUSTAIN 0x20
#define MASK_VIBRATO 0x40

enum { S_OFF, S_RELEASE, S_SUSTAIN, S_DECAY, S_ATTACK };
/* Same order as DBOPL's SynthMode: the code compares against 4START/6START. */
enum { SM_2AM, SM_2FM, SM_3AM, SM_3FM, SM_4START, SM_3FMFM, SM_3AMFM, SM_3FMAM, SM_3AMAM,
       SM_6START, SM_2PERC, SM_3PERC };

static const uint8_t ksl_create[16] = { 64, 32, 24, 19, 16, 12, 11, 10, 8, 6, 5, 4, 3, 2, 1, 0 };
/* FreqCreateTable: the multipliers times two (0.5 -> 1). */
static const uint8_t freq_create[16] = { 1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30 };
static const uint16_t wave_base_table[8] = { 0x000, 0x200, 0x200, 0x800, 0xa00, 0xc00, 0x100, 0x400 };
static const uint16_t wave_mask_table[8] = { 1023, 1023, 511, 511, 1023, 1023, 512, 1023 };
static const uint16_t wave_start_table[8] = { 512, 0, 0, 0, 0, 512, 512, 256 };
static const int8_t vibrato_table[8] = { 1, 0, 1, 30, 1 - 0x80, 0 - 0x80, 1 - 0x80, 30 - 0x80 };
static const uint8_t ksl_shift_table[4] = { 31, 1, 2, 0 };

static int16_t wave_table[8 * 512];
static uint8_t ksl_table[8 * 16], tremolo_table[TREMOLO_TABLE];
static uint8_t chan_index[32]; /* register low bits -> channel + 1, 0 none */
static uint8_t op_index[64];   /* register low bits -> channel * 2 + operator + 1 */
static bool tables_ready;

static void tables_init(void)
{
    if (tables_ready) return;
    for (unsigned i = 0; i < 512; i++) {
        wave_table[0x200 + i] = opl3_sine_table[i];
        wave_table[0x000 + i] = (int16_t)-opl3_sine_table[i];
    }
    for (unsigned i = 0; i < 256; i++) {
        wave_table[0x700 + i] = opl3_exp_table[i];
        wave_table[0x6ff - i] = (int16_t)-opl3_exp_table[i];
    }
    for (unsigned i = 0; i < 256; i++) {
        wave_table[0x400 + i] = wave_table[0];
        wave_table[0x500 + i] = wave_table[0];
        wave_table[0x900 + i] = wave_table[0];
        wave_table[0xc00 + i] = wave_table[0];
        wave_table[0xd00 + i] = wave_table[0];
        wave_table[0x800 + i] = wave_table[0x200 + i];
        wave_table[0xa00 + i] = wave_table[0x200 + i * 2];
        wave_table[0xb00 + i] = wave_table[0x000 + i * 2];
        wave_table[0xe00 + i] = wave_table[0x200 + i * 2];
        wave_table[0xf00 + i] = wave_table[0x200 + i * 2];
    }
    for (int oct = 0; oct < 8; oct++) {
        int base = oct * 8;
        for (int i = 0; i < 16; i++) {
            int val = base - ksl_create[i];
            if (val < 0) val = 0;
            ksl_table[oct * 16 + i] = (uint8_t)(val * 4);
        }
    }
    for (unsigned i = 0; i < TREMOLO_TABLE / 2; i++) {
        uint8_t val = (uint8_t)(i << ENV_EXTRA);
        tremolo_table[i] = val;
        tremolo_table[TREMOLO_TABLE - 1 - i] = val;
    }
    for (unsigned i = 0; i < 32; i++) {
        unsigned index = i & 0xf;
        if (index >= 9) { chan_index[i] = 0; continue; }
        if (index < 6) index = (index % 3) * 2 + index / 3; /* four-op channels follow each other */
        if (i >= 16) index += 9;
        chan_index[i] = (uint8_t)(index + 1);
    }
    for (unsigned i = 0; i < 64; i++) {
        if (i % 8 >= 6 || (i / 8) % 4 == 3) { op_index[i] = 0; continue; }
        unsigned ch = (i / 8) * 3 + (i % 8) % 3;
        if (ch >= 12) ch += 16 - 12;
        unsigned op = (i % 8) / 3;
        op_index[i] = (uint8_t)((chan_index[ch] - 1) * 2 + op + 1);
    }
    tables_ready = true;
}

/* ---- Operator ---- */
static inline struct opl3_op *chop(struct opl3_chan *ch, unsigned index)
{
    return &(ch + (index >> 1))->op[index & 1];
}
static void update_attack(struct opl3_op *o)
{
    uint8_t rate = o->reg60 >> 4;
    if (rate) {
        o->attack_add = opl3_attack_rates[(rate << 2) + o->ksr];
        o->rate_zero &= (uint8_t)~(1u << S_ATTACK);
    } else {
        o->attack_add = 0;
        o->rate_zero |= 1u << S_ATTACK;
    }
}
static void update_decay(struct opl3_op *o)
{
    uint8_t rate = o->reg60 & 0xf;
    if (rate) {
        o->decay_add = opl3_linear_rates[(rate << 2) + o->ksr];
        o->rate_zero &= (uint8_t)~(1u << S_DECAY);
    } else {
        o->decay_add = 0;
        o->rate_zero |= 1u << S_DECAY;
    }
}
static void update_release(struct opl3_op *o)
{
    uint8_t rate = o->reg80 & 0xf;
    if (rate) {
        o->release_add = opl3_linear_rates[(rate << 2) + o->ksr];
        o->rate_zero &= (uint8_t)~(1u << S_RELEASE);
        if (!(o->reg20 & MASK_SUSTAIN)) o->rate_zero &= (uint8_t)~(1u << S_SUSTAIN);
    } else {
        o->rate_zero |= 1u << S_RELEASE;
        o->release_add = 0;
        if (!(o->reg20 & MASK_SUSTAIN)) o->rate_zero |= 1u << S_SUSTAIN;
    }
}
static void update_attenuation(struct opl3_op *o)
{
    uint8_t ksl_base = (uint8_t)((o->chan_data >> SHIFT_KSLBASE) & 0xff);
    uint32_t tl = o->reg40 & 0x3f;
    uint8_t ksl_shift = ksl_shift_table[o->reg40 >> 6];
    o->total_level = (int32_t)(tl << (ENV_BITS - 7));
    o->total_level += (ksl_base << ENV_EXTRA) >> ksl_shift;
}
static void update_frequency(struct opl3_op *o)
{
    uint32_t freq = o->chan_data & ((1u << 10) - 1);
    uint32_t block = (o->chan_data >> 10) & 0xff;
    o->wave_add = (freq << block) * o->freq_mul;
    if (o->reg20 & MASK_VIBRATO) {
        o->vib_strength = (uint8_t)(freq >> 7);
        o->vibrato = ((uint32_t)o->vib_strength << block) * o->freq_mul;
    } else {
        o->vib_strength = 0;
        o->vibrato = 0;
    }
}
static void update_rates(struct opl3_op *o)
{
    uint8_t ksr = (uint8_t)((o->chan_data >> SHIFT_KEYCODE) & 0xff);
    if (!(o->reg20 & MASK_KSR)) ksr >>= 2;
    if (o->ksr == ksr) return;
    o->ksr = ksr;
    update_attack(o); update_decay(o); update_release(o);
}
static inline int32_t rate_forward(struct opl3_op *o, uint32_t add)
{
    o->rate_index += add;
    int32_t ret = (int32_t)(o->rate_index >> RATE_SH);
    o->rate_index &= RATE_MASK;
    return ret;
}
static inline int32_t envelope(struct opl3_op *o)
{
    int32_t vol = o->volume, change;
    switch (o->state) {
    case S_OFF:
        return ENV_MAX;
    case S_ATTACK:
        change = rate_forward(o, o->attack_add);
        if (!change) return vol;
        vol += ((~vol) * change) >> 3;
        if (vol < ENV_MIN) {
            o->volume = ENV_MIN; o->rate_index = 0; o->state = S_DECAY;
            return ENV_MIN;
        }
        break;
    case S_DECAY:
        vol += rate_forward(o, o->decay_add);
        if (vol >= o->sustain_level) {
            if (vol >= ENV_MAX) { o->volume = ENV_MAX; o->state = S_OFF; return ENV_MAX; }
            o->rate_index = 0; o->state = S_SUSTAIN;
        }
        break;
    case S_SUSTAIN:
        if (o->reg20 & MASK_SUSTAIN) return vol;
        __attribute__((fallthrough)); /* not sustaining: regular release */
    case S_RELEASE:
        vol += rate_forward(o, o->release_add);
        if (vol >= ENV_MAX) { o->volume = ENV_MAX; o->state = S_OFF; return ENV_MAX; }
        break;
    }
    o->volume = vol;
    return vol;
}
static inline uint32_t forward_volume(struct opl3_op *o) { return o->current_level + (uint32_t)envelope(o); }
static inline uint32_t forward_wave(struct opl3_op *o)
{
    o->wave_index += o->wave_current;
    return o->wave_index >> WAVE_SH;
}
static inline int32_t get_wave(const struct opl3_op *o, uint32_t index, uint32_t vol)
{
    return (wave_table[o->wave_base + (index & o->wave_mask)] * opl3_mul_table[vol >> ENV_EXTRA]) >> MUL_SH;
}
static inline int32_t get_sample(struct opl3_op *o, int32_t modulation)
{
    uint32_t vol = forward_volume(o);
    if (ENV_SILENT(vol)) {
        o->wave_index += o->wave_current; /* simply forward the wave */
        return 0;
    }
    uint32_t index = forward_wave(o) + (uint32_t)modulation;
    return get_wave(o, index, vol);
}
static void write20(struct opl3_op *o, uint8_t val)
{
    uint8_t change = o->reg20 ^ val;
    if (!change) return;
    o->reg20 = val;
    o->tremolo_mask = (uint8_t)((int8_t)val >> 7); /* the tremolo bit over the whole byte */
    o->tremolo_mask &= (uint8_t)~((1u << ENV_EXTRA) - 1);
    if (change & MASK_KSR) update_rates(o);
    if ((o->reg20 & MASK_SUSTAIN) || !o->release_add) o->rate_zero |= 1u << S_SUSTAIN;
    else o->rate_zero &= (uint8_t)~(1u << S_SUSTAIN);
    if (change & (0xf | MASK_VIBRATO)) {
        o->freq_mul = OPL3_FREQ_SCALE * freq_create[val & 0xf];
        update_frequency(o);
    }
}
static void write40(struct opl3_op *o, uint8_t val)
{
    if (!(o->reg40 ^ val)) return;
    o->reg40 = val;
    update_attenuation(o);
}
static void write60(struct opl3_op *o, uint8_t val)
{
    uint8_t change = o->reg60 ^ val;
    o->reg60 = val;
    if (change & 0x0f) update_decay(o);
    if (change & 0xf0) update_attack(o);
}
static void write80(struct opl3_op *o, uint8_t val)
{
    uint8_t change = o->reg80 ^ val;
    if (!change) return;
    o->reg80 = val;
    uint8_t sustain = val >> 4;
    sustain |= (uint8_t)((sustain + 1) & 0x10); /* 0xf becomes 0x1f */
    o->sustain_level = sustain << (ENV_BITS - 5);
    if (change & 0x0f) update_release(o);
}
static void writeE0(const struct opl3_chip *c, struct opl3_op *o, uint8_t val)
{
    if (!(o->regE0 ^ val)) return;
    /* An OPL3 always allows all eight waveforms in OPL3 mode. */
    uint8_t form = (uint8_t)(val & ((0x3 & c->wave_form_mask) | (0x7 & c->opl3_active)));
    o->regE0 = val;
    o->wave_base = wave_base_table[form];
    o->wave_start = (uint32_t)wave_start_table[form] << WAVE_SH;
    o->wave_mask = wave_mask_table[form];
}
static bool op_silent(const struct opl3_op *o)
{
    if (!ENV_SILENT(o->total_level + o->volume)) return false;
    if (!(o->rate_zero & (1u << o->state))) return false;
    return true;
}
static inline void prepare(const struct opl3_chip *c, struct opl3_op *o)
{
    o->current_level = (uint32_t)(o->total_level + (int32_t)(c->tremolo_value & o->tremolo_mask));
    o->wave_current = o->wave_add;
    if (o->vib_strength >> c->vibrato_shift) {
        int32_t add = (int32_t)(o->vibrato >> c->vibrato_shift);
        int32_t neg = c->vibrato_sign; /* sign extended over the shift value */
        add = (add ^ neg) - neg;
        o->wave_current += (uint32_t)add;
    }
}
static void key_on(struct opl3_op *o, uint8_t mask)
{
    if (!o->key_on) {
        o->wave_index = o->wave_start; /* restart the frequency generator */
        o->rate_index = 0;
        o->state = S_ATTACK;
    }
    o->key_on |= mask;
}
static void key_off(struct opl3_op *o, uint8_t mask)
{
    o->key_on &= (uint8_t)~mask;
    if (!o->key_on && o->state != S_OFF) o->state = S_RELEASE;
}

/* ---- Channel ---- */
static void set_chan_data(struct opl3_chan *ch, uint32_t data)
{
    uint32_t change = ch->chan_data ^ data;
    ch->chan_data = data;
    ch->op[0].chan_data = data;
    ch->op[1].chan_data = data;
    update_frequency(&ch->op[0]);
    update_frequency(&ch->op[1]);
    if (change & (0xffu << SHIFT_KSLBASE)) { update_attenuation(&ch->op[0]); update_attenuation(&ch->op[1]); }
    if (change & (0xffu << SHIFT_KEYCODE)) { update_rates(&ch->op[0]); update_rates(&ch->op[1]); }
}
static void chan_update_frequency(const struct opl3_chip *c, struct opl3_chan *ch, uint8_t four_op)
{
    uint32_t data = ch->chan_data & 0xffff;
    uint32_t ksl_base = ksl_table[data >> 6];
    uint32_t key_code = (data & 0x1c00) >> 9;
    if (c->reg08 & 0x40) key_code |= (data & 0x100) >> 8; /* notesel 1 */
    else key_code |= (data & 0x200) >> 9;                 /* notesel 0 */
    data |= (key_code << SHIFT_KEYCODE) | (ksl_base << SHIFT_KSLBASE);
    set_chan_data(ch, data);
    if (four_op & 0x3f) set_chan_data(ch + 1, data);
}
static uint8_t four_op_of(const struct opl3_chip *c, const struct opl3_chan *ch)
{
    return (uint8_t)(c->reg104 & c->opl3_active & ch->four_mask);
}
static void writeA0(struct opl3_chip *c, struct opl3_chan *ch, uint8_t val)
{
    uint8_t four_op = four_op_of(c, ch);
    if (four_op > 0x80) return; /* the silent second channel of a four-op pair */
    uint32_t change = (ch->chan_data ^ val) & 0xff;
    if (change) { ch->chan_data ^= change; chan_update_frequency(c, ch, four_op); }
}
static void writeB0(struct opl3_chip *c, struct opl3_chan *ch, uint8_t val)
{
    uint8_t four_op = four_op_of(c, ch);
    if (four_op > 0x80) return;
    uint32_t change = (ch->chan_data ^ ((uint32_t)val << 8)) & 0x1f00u;
    if (change) { ch->chan_data ^= change; chan_update_frequency(c, ch, four_op); }
    if (!((val ^ ch->regB0) & 0x20)) return; /* no key on/off change */
    ch->regB0 = val;
    if (val & 0x20) {
        key_on(&ch->op[0], 1); key_on(&ch->op[1], 1);
        if (four_op & 0x3f) { key_on(&(ch + 1)->op[0], 1); key_on(&(ch + 1)->op[1], 1); }
    } else {
        key_off(&ch->op[0], 1); key_off(&ch->op[1], 1);
        if (four_op & 0x3f) { key_off(&(ch + 1)->op[0], 1); key_off(&(ch + 1)->op[1], 1); }
    }
}
static void update_synth(const struct opl3_chip *c, struct opl3_chan *ch)
{
    if (c->opl3_active) {
        if ((c->reg104 & ch->four_mask) & 0x3f) {
            struct opl3_chan *c0, *c1;
            if (!(ch->four_mask & 0x80)) { c0 = ch; c1 = ch + 1; }
            else { c0 = ch - 1; c1 = ch; }
            switch ((c0->regC0 & 1) | ((c1->regC0 & 1) << 1)) {
            case 0: c0->mode = SM_3FMFM; break;
            case 1: c0->mode = SM_3AMFM; break;
            case 2: c0->mode = SM_3FMAM; break;
            default: c0->mode = SM_3AMAM; break;
            }
        } else if ((ch->four_mask & 0x40) && (c->regBD & 0x20)) {
            /* percussion channels keep their handler */
        } else if (ch->regC0 & 1) {
            ch->mode = SM_3AM;
        } else {
            ch->mode = SM_3FM;
        }
        ch->mask_left = (ch->regC0 & 0x10) ? -1 : 0;
        ch->mask_right = (ch->regC0 & 0x20) ? -1 : 0;
    } else {
        if ((ch->four_mask & 0x40) && (c->regBD & 0x20)) {
            /* percussion channels keep their handler */
        } else if (ch->regC0 & 1) {
            ch->mode = SM_2AM;
        } else {
            ch->mode = SM_2FM;
        }
    }
}
static void writeC0(const struct opl3_chip *c, struct opl3_chan *ch, uint8_t val)
{
    uint8_t change = val ^ ch->regC0;
    if (!change) return;
    ch->regC0 = val;
    ch->feedback = (ch->regC0 >> 1) & 7;
    ch->feedback = ch->feedback ? (uint8_t)(9 - ch->feedback) : 31; /* shift onto the 10-bit wave index */
    update_synth(c, ch);
}

/* ---- Chip ---- */
static uint32_t forward_noise(struct opl3_chip *c)
{
    c->noise_counter += c->noise_add;
    uint32_t count = c->noise_counter >> LFO_SH;
    c->noise_counter &= (1u << LFO_SH) - 1;
    for (; count > 0; --count) {
        c->noise_value ^= 0x800302u & (0u - (c->noise_value & 1)); /* noise calculation from MAME */
        c->noise_value >>= 1;
    }
    return c->noise_value;
}
static uint32_t forward_lfo(struct opl3_chip *c, uint32_t samples)
{
    c->vibrato_sign = (int8_t)(vibrato_table[c->vibrato_index >> 2] >> 7);
    c->vibrato_shift = (uint8_t)((vibrato_table[c->vibrato_index >> 2] & 7) + c->vibrato_strength);
    c->tremolo_value = tremolo_table[c->tremolo_index] >> c->tremolo_strength;
    uint32_t todo = LFO_MAX - c->lfo_counter;
    uint32_t count = (todo + c->lfo_add - 1) / c->lfo_add;
    if (count > samples) {
        count = samples;
        c->lfo_counter += count * c->lfo_add;
    } else {
        c->lfo_counter += count * c->lfo_add;
        c->lfo_counter &= LFO_MAX - 1;
        c->vibrato_index = (c->vibrato_index + 1) & 31;
        if (c->tremolo_index + 1 < TREMOLO_TABLE) ++c->tremolo_index;
        else c->tremolo_index = 0;
    }
    return count;
}
static void generate_percussion(struct opl3_chip *c, struct opl3_chan *ch, int32_t *output, bool opl3)
{
    /* Bass drum */
    int32_t mod = (int32_t)((uint32_t)(ch->old[0] + ch->old[1]) >> ch->feedback);
    ch->old[0] = ch->old[1];
    ch->old[1] = get_sample(chop(ch, 0), mod);
    mod = (ch->regC0 & 1) ? 0 : ch->old[0]; /* AM mode ignores the first operator */
    int32_t sample = get_sample(chop(ch, 1), mod);
    /* Values shared by the other outputs */
    uint32_t noise_bit = forward_noise(c) & 0x1;
    uint32_t c2 = forward_wave(chop(ch, 2));
    uint32_t c5 = forward_wave(chop(ch, 5));
    uint32_t phase_bit = (((c2 & 0x88) ^ ((c2 << 5) & 0x80)) | ((c5 ^ (c5 << 2)) & 0x20)) ? 0x02 : 0x00;
    /* Hi-hat */
    uint32_t hh_vol = forward_volume(chop(ch, 2));
    if (!ENV_SILENT(hh_vol)) {
        uint32_t hh_index = (phase_bit << 8) | (0x34u << (phase_bit ^ (noise_bit << 1)));
        sample += get_wave(chop(ch, 2), hh_index, hh_vol);
    }
    /* Snare drum */
    uint32_t sd_vol = forward_volume(chop(ch, 3));
    if (!ENV_SILENT(sd_vol)) {
        uint32_t sd_index = (0x100 + (c2 & 0x100)) ^ (noise_bit << 8);
        sample += get_wave(chop(ch, 3), sd_index, sd_vol);
    }
    /* Tom-tom */
    sample += get_sample(chop(ch, 4), 0);
    /* Top cymbal */
    uint32_t tc_vol = forward_volume(chop(ch, 5));
    if (!ENV_SILENT(tc_vol)) {
        uint32_t tc_index = (1 + phase_bit) << 8;
        sample += get_wave(chop(ch, 5), tc_index, tc_vol);
    }
    sample *= 2;
    if (opl3) { output[0] += sample; output[1] += sample; }
    else output[0] += sample;
}
/* The per-sample synthesis of BlockTemplate; `mode` is a constant at each
 * call site so the compiler drops the untaken branches. */
static inline __attribute__((always_inline))
void synth_run(struct opl3_chan *ch, uint32_t samples, int32_t *output, const uint8_t mode)
{
    for (uint32_t i = 0; i < samples; i++) {
        /* Unsigned shift: shift out every bit but keep the 10-bit range otherwise */
        int32_t mod = (int32_t)((uint32_t)(ch->old[0] + ch->old[1]) >> ch->feedback);
        ch->old[0] = ch->old[1];
        ch->old[1] = get_sample(chop(ch, 0), mod);
        int32_t out0 = ch->old[0], sample = 0, next;
        if (mode == SM_2AM || mode == SM_3AM) {
            sample = out0 + get_sample(chop(ch, 1), 0);
        } else if (mode == SM_2FM || mode == SM_3FM) {
            sample = get_sample(chop(ch, 1), out0);
        } else if (mode == SM_3FMFM) {
            next = get_sample(chop(ch, 1), out0);
            next = get_sample(chop(ch, 2), next);
            sample = get_sample(chop(ch, 3), next);
        } else if (mode == SM_3AMFM) {
            sample = out0;
            next = get_sample(chop(ch, 1), 0);
            next = get_sample(chop(ch, 2), next);
            sample += get_sample(chop(ch, 3), next);
        } else if (mode == SM_3FMAM) {
            sample = get_sample(chop(ch, 1), out0);
            next = get_sample(chop(ch, 2), 0);
            sample += get_sample(chop(ch, 3), next);
        } else if (mode == SM_3AMAM) {
            sample = out0;
            next = get_sample(chop(ch, 1), 0);
            sample += get_sample(chop(ch, 2), next);
            sample += get_sample(chop(ch, 3), 0);
        }
        if (mode == SM_2AM || mode == SM_2FM) {
            output[i] += sample;
        } else {
            output[i * 2 + 0] += sample & ch->mask_left;
            output[i * 2 + 1] += sample & ch->mask_right;
        }
    }
}
static struct opl3_chan *block(struct opl3_chip *c, struct opl3_chan *ch, uint32_t samples, int32_t *output)
{
    const uint8_t mode = ch->mode;
    switch (mode) {
    case SM_2AM: case SM_3AM:
        if (op_silent(chop(ch, 0)) && op_silent(chop(ch, 1))) { ch->old[0] = ch->old[1] = 0; return ch + 1; }
        break;
    case SM_2FM: case SM_3FM:
        if (op_silent(chop(ch, 1))) { ch->old[0] = ch->old[1] = 0; return ch + 1; }
        break;
    case SM_3FMFM:
        if (op_silent(chop(ch, 3))) { ch->old[0] = ch->old[1] = 0; return ch + 2; }
        break;
    case SM_3AMFM:
        if (op_silent(chop(ch, 0)) && op_silent(chop(ch, 3))) { ch->old[0] = ch->old[1] = 0; return ch + 2; }
        break;
    case SM_3FMAM:
        if (op_silent(chop(ch, 1)) && op_silent(chop(ch, 3))) { ch->old[0] = ch->old[1] = 0; return ch + 2; }
        break;
    case SM_3AMAM:
        if (op_silent(chop(ch, 0)) && op_silent(chop(ch, 2)) && op_silent(chop(ch, 3))) {
            ch->old[0] = ch->old[1] = 0; return ch + 2;
        }
        break;
    default:
        break;
    }
    /* Init the operators with the current vibrato and tremolo values */
    prepare(c, chop(ch, 0));
    prepare(c, chop(ch, 1));
    if (mode > SM_4START) { prepare(c, chop(ch, 2)); prepare(c, chop(ch, 3)); }
    if (mode > SM_6START) { prepare(c, chop(ch, 4)); prepare(c, chop(ch, 5)); }
    switch (mode) {
    case SM_2PERC:
        for (uint32_t i = 0; i < samples; i++) generate_percussion(c, ch, output + i, false);
        return ch + 3;
    case SM_3PERC:
        for (uint32_t i = 0; i < samples; i++) generate_percussion(c, ch, output + i * 2, true);
        return ch + 3;
    case SM_2AM: synth_run(ch, samples, output, SM_2AM); return ch + 1;
    case SM_2FM: synth_run(ch, samples, output, SM_2FM); return ch + 1;
    case SM_3AM: synth_run(ch, samples, output, SM_3AM); return ch + 1;
    case SM_3FM: synth_run(ch, samples, output, SM_3FM); return ch + 1;
    case SM_3FMFM: synth_run(ch, samples, output, SM_3FMFM); return ch + 2;
    case SM_3AMFM: synth_run(ch, samples, output, SM_3AMFM); return ch + 2;
    case SM_3FMAM: synth_run(ch, samples, output, SM_3FMAM); return ch + 2;
    case SM_3AMAM: synth_run(ch, samples, output, SM_3AMAM); return ch + 2;
    default: return ch + 1; /* unreachable: every mode above is handled */
    }
}
static void update_synths(struct opl3_chip *c)
{
    for (unsigned i = 0; i < 18; i++) update_synth(c, &c->chan[i]);
}
static void write_bd(struct opl3_chip *c, uint8_t val)
{
    uint8_t change = c->regBD ^ val;
    if (!change) return;
    c->regBD = val;
    c->vibrato_strength = (val & 0x40) ? 0x00 : 0x01;
    c->tremolo_strength = (val & 0x80) ? 0x00 : 0x02;
    struct opl3_chan *ch = c->chan;
    if (val & 0x20) {
        /* Drums just enabled: channel 6 gets the percussion handler */
        if (change & 0x20) ch[6].mode = c->opl3_active ? SM_3PERC : SM_2PERC;
        if (val & 0x10) { key_on(&ch[6].op[0], 2); key_on(&ch[6].op[1], 2); }  /* bass drum */
        else { key_off(&ch[6].op[0], 2); key_off(&ch[6].op[1], 2); }
        if (val & 0x1) key_on(&ch[7].op[0], 2); else key_off(&ch[7].op[0], 2); /* hi-hat */
        if (val & 0x8) key_on(&ch[7].op[1], 2); else key_off(&ch[7].op[1], 2); /* snare */
        if (val & 0x4) key_on(&ch[8].op[0], 2); else key_off(&ch[8].op[0], 2); /* tom-tom */
        if (val & 0x2) key_on(&ch[8].op[1], 2); else key_off(&ch[8].op[1], 2); /* top cymbal */
    } else if (change & 0x20) {
        /* Percussion off: restore the original handler and release the drums */
        update_synth(c, &ch[6]);
        key_off(&ch[6].op[0], 2); key_off(&ch[6].op[1], 2);
        key_off(&ch[7].op[0], 2); key_off(&ch[7].op[1], 2);
        key_off(&ch[8].op[0], 2); key_off(&ch[8].op[1], 2);
    }
}
static struct opl3_op *reg_op(struct opl3_chip *c, unsigned reg)
{
    unsigned index = op_index[((reg >> 3) & 0x20) | (reg & 0x1f)];
    return index ? &c->chan[(index - 1) / 2].op[(index - 1) % 2] : 0;
}
static struct opl3_chan *reg_chan(struct opl3_chip *c, unsigned reg)
{
    unsigned index = chan_index[((reg >> 4) & 0x10) | (reg & 0xf)];
    return index ? &c->chan[index - 1] : 0;
}
void opl3_write(struct opl3_chip *c, unsigned reg, uint8_t val)
{
    struct opl3_op *op;
    struct opl3_chan *ch;
    reg &= 0x1ff;
    switch ((reg & 0xf0) >> 4) {
    case 0x0:
        if (reg == 0x01) {
            c->wave_form_mask = 0x7; /* an OPL3 cannot disable the waveforms (opl3Mode) */
        } else if (reg == 0x104) {
            if (!((c->reg104 ^ val) & 0x3f)) return; /* only the lowest six bits */
            c->reg104 = (uint8_t)(0x80 | (val & 0x3f));  /* highest bit kept for the > 0x80 checks */
            update_synths(c);
        } else if (reg == 0x105) {
            if (!((c->opl3_active ^ val) & 1)) return;
            c->opl3_active = (val & 1) ? -1 : 0;
            update_synths(c);
        } else if (reg == 0x08) {
            c->reg08 = val;
        }
        break;
    case 0x1:
        break;
    case 0x2: case 0x3:
        if ((op = reg_op(c, reg))) write20(op, val);
        break;
    case 0x4: case 0x5:
        if ((op = reg_op(c, reg))) write40(op, val);
        break;
    case 0x6: case 0x7:
        if ((op = reg_op(c, reg))) write60(op, val);
        break;
    case 0x8: case 0x9:
        if ((op = reg_op(c, reg))) write80(op, val);
        break;
    case 0xa:
        if ((ch = reg_chan(c, reg))) writeA0(c, ch, val);
        break;
    case 0xb:
        if (reg == 0xbd) write_bd(c, val);
        else if ((ch = reg_chan(c, reg))) writeB0(c, ch, val);
        break;
    case 0xc:
        if ((ch = reg_chan(c, reg))) writeC0(c, ch, val);
        break;
    case 0xd:
        break;
    default: /* 0xe, 0xf */
        if ((op = reg_op(c, reg))) writeE0(c, op, val);
        break;
    }
}
static void generate_block2(struct opl3_chip *c, uint32_t total, int32_t *output)
{
    while (total > 0) {
        uint32_t samples = forward_lfo(c, total);
        memset(output, 0, sizeof(int32_t) * samples);
        for (struct opl3_chan *ch = c->chan; ch < c->chan + 9;) ch = block(c, ch, samples, output);
        total -= samples;
        output += samples;
    }
}
static void generate_block3(struct opl3_chip *c, uint32_t total, int32_t *output)
{
    while (total > 0) {
        uint32_t samples = forward_lfo(c, total);
        memset(output, 0, sizeof(int32_t) * samples * 2);
        for (struct opl3_chan *ch = c->chan; ch < c->chan + 18;) ch = block(c, ch, samples, output);
        total -= samples;
        output += samples * 2;
    }
}
void opl3_generate(struct opl3_chip *c, int32_t *stereo, unsigned frames)
{
    if (!frames) return;
    if (!c->opl3_active) {
        /* Mono into the first half (a percussion handler left from OPL3 mode
         * may write up to twice as far, still inside the stereo buffer), then
         * expanded backwards onto both channels. */
        generate_block2(c, frames, stereo);
        for (unsigned i = frames; i; --i) {
            int32_t sample = stereo[i - 1];
            stereo[(i - 1) * 2] = sample;
            stereo[(i - 1) * 2 + 1] = sample;
        }
    } else {
        generate_block3(c, frames, stereo);
    }
}
bool opl3_silent(const struct opl3_chip *c)
{
    /* Outside ATTACK the attenuation only grows until the next key on. */
    for (unsigned i = 0; i < 18; i++)
        for (unsigned j = 0; j < 2; j++) {
            const struct opl3_op *o = &c->chan[i].op[j];
            if (o->state == S_ATTACK || !ENV_SILENT(o->total_level + o->volume)) return false;
        }
    return true;
}
void opl3_init(struct opl3_chip *c)
{
    tables_init();
    memset(c, 0, sizeof(*c));
    for (unsigned i = 0; i < 18; i++) {
        struct opl3_chan *ch = &c->chan[i];
        ch->mask_left = ch->mask_right = -1;
        ch->feedback = 31;
        ch->mode = SM_2FM;
        for (unsigned j = 0; j < 2; j++) {
            struct opl3_op *o = &ch->op[j];
            o->state = S_OFF;
            o->rate_zero = 1u << S_OFF;
            o->sustain_level = ENV_MAX;
            o->current_level = ENV_MAX;
            o->total_level = ENV_MAX;
            o->volume = ENV_MAX;
        }
    }
    /* Chip::Setup(48000): the rate-dependent values come from opl3_tables.h. */
    c->noise_add = OPL3_LFO_ADD;
    c->noise_counter = 0;
    c->noise_value = 1; /* triggers the noise xor the first time */
    c->lfo_add = OPL3_LFO_ADD;
    c->lfo_counter = 0;
    c->vibrato_index = c->tremolo_index = 0;
    static const uint8_t four[18] = { 0x01, 0x81, 0x02, 0x82, 0x04, 0x84, 0x40, 0x40, 0x40,
                                      0x08, 0x88, 0x10, 0x90, 0x20, 0xa0, 0x00, 0x00, 0x00 };
    for (unsigned i = 0; i < 18; i++) c->chan[i].four_mask = four[i];
    /* Clear everything in OPL3 mode, then in OPL2 mode */
    opl3_write(c, 0x105, 0x1);
    for (unsigned i = 0; i < 512; i++) {
        if (i == 0x105) continue;
        opl3_write(c, i, 0xff);
        opl3_write(c, i, 0x0);
    }
    opl3_write(c, 0x105, 0x0);
    for (unsigned i = 0; i < 255; i++) {
        opl3_write(c, i, 0xff);
        opl3_write(c, i, 0x0);
    }
}
