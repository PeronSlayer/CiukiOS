/* Integer OPL3 (YMF262) synthesizer for DOS VM audio (f4-05): a C port of
 * DOSBox's DBOPL with its floating-point tables precomputed for 48000 Hz.
 *
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
 *
 * Source: third_party/vsbhda (VSBHDA 75fa4bb, src/DBOPL.CPP and DBOPL.H,
 * unmodified upstream). Decisions: docs/design/f4-acceptance.md, f4-05.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CIUKI_OPL3_H
#define CIUKI_OPL3_H
#include <stdint.h>
#include <stdbool.h>

#define OPL3_RATE 48000u /* output frames per second (the mixer rate) */

struct opl3_op {
    uint32_t wave_base;     /* offset of the waveform in the shared wave table */
    uint32_t wave_mask, wave_start;
    uint32_t wave_index, wave_add, wave_current;
    uint32_t chan_data, freq_mul, vibrato;
    int32_t sustain_level, total_level;
    uint32_t current_level;
    int32_t volume;
    uint32_t attack_add, decay_add, release_add, rate_index;
    uint8_t rate_zero, key_on, state;
    uint8_t reg20, reg40, reg60, reg80, regE0;
    uint8_t tremolo_mask, vib_strength, ksr;
};

struct opl3_chan {
    struct opl3_op op[2]; /* first: pointer arithmetic over adjacent channels */
    uint32_t chan_data;
    int32_t old[2];
    uint8_t feedback, regB0, regC0, four_mask, mode;
    int8_t mask_left, mask_right;
};

struct opl3_chip {
    struct opl3_chan chan[18];
    uint32_t lfo_counter, lfo_add, noise_counter, noise_add, noise_value;
    uint8_t reg104, reg08, reg04, regBD, vibrato_index, tremolo_index;
    int8_t vibrato_sign;
    uint8_t vibrato_shift, tremolo_value, vibrato_strength, tremolo_strength, wave_form_mask;
    int8_t opl3_active; /* 0 or -1 (NEW set) */
};

/* Chip(true) followed by Setup(48000): every register cleared in OPL3 and
 * then OPL2 mode, as DBOPL does. No allocation, no FPU. */
void opl3_init(struct opl3_chip *chip);
/* One register write, index 000h-1FFh (array 1 at 100h). */
void opl3_write(struct opl3_chip *chip, unsigned reg, uint8_t value);
/* Generates frames of interleaved stereo int32 samples (unclipped, DBOPL's
 * scale: one full-level operator is about +-4084). OPL2 mode is mono on both
 * channels; OPL3 mode honours C0h bits 4 (left) and 5 (right). */
void opl3_generate(struct opl3_chip *chip, int32_t *stereo, unsigned frames);
/* True when every operator is off (the chip produces exact silence). */
bool opl3_silent(const struct opl3_chip *chip);
#endif
