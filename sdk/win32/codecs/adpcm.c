/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2001,2002 Eric Pouech (reference tables/predictors).
 * Copyright 2026 CiukiOS contributors (bounded implementation).
 * See third_party/wine/COPYING.LIB and SOURCE-OFFER.txt.
 * Bounded WAVE ADPCM decoding; IMA revision 3 and Microsoft RIFF update.
 * Tables and predictor arithmetic agree with pinned Wine 10.0 (Eric Pouech).
 */
#include "codec.h"
static const int steps[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
    19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int indices[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
static const int adapt[16] = {230, 230, 230, 230, 307, 409, 512, 614,
                              768, 614, 512, 409, 307, 230, 230, 230};
uint32_t cc_wave_validate(const struct cc_wave *f, uint32_t *samples) {
  uint32_t ch, header, n;
  if (f && samples && f->tag == 1 && (f->channels == 1 || f->channels == 2) &&
      f->rate && f->rate <= 192000 && (f->bits == 8 || f->bits == 16) &&
      f->align == f->channels * (f->bits / 8)) {
    *samples = 1;
    return 0;
  }
  if (!f || !samples || !(f->channels == 1 || f->channels == 2) || !f->rate ||
      f->rate > 192000 || f->bits != 4 || (f->tag != 2 && f->tag != 17))
    return CC_NOTPOSSIBLE;
  ch = f->channels;
  header = (f->tag == 17 ? 4u : 7u) * ch;
  if (f->align < header || f->extra < (f->tag == 17 ? 2 : 32))
    return CC_NOTPOSSIBLE;
  n = (f->align - header) * 2 / ch + (f->tag == 17 ? 1 : 2);
  if (cc_le16((const uint8_t *)f + 18) != n)
    return CC_NOTPOSSIBLE;
  if (f->tag == 17 && (f->align - header) % (4 * ch))
    return CC_NOTPOSSIBLE;
  if (f->tag == 2) {
    uint32_t count = cc_le16((const uint8_t *)f + 20);
    if (count < 7 || count > 256 || f->extra < 4 + 4 * count)
      return CC_NOTPOSSIBLE;
    /* Microsoft RIFF update, pp. 22–23: the seven preset pairs must
     * appear in order, even when only one predictor is used. */
    static const int16_t standard[7][2] = {
        {256, 0}, {512, -256}, {0, 0}, {192, 64},
        {240, 0}, {460, -208}, {392, -232}};
    const uint8_t *pairs = (const uint8_t *)f + 22;
    for (unsigned i = 0; i < 7; i++)
      if ((int16_t)cc_le16(pairs + 4*i) != standard[i][0] ||
          (int16_t)cc_le16(pairs + 4*i + 2) != standard[i][1])
        return CC_NOTPOSSIBLE;
  }
  *samples = n;
  return CC_OK;
}
uint32_t cc_suggest_format(const struct cc_wave *s, struct cc_wave *d,
                           uint32_t bytes, uint32_t flags) {
  uint32_t samples;
  if (!d || bytes < 16 || flags & ~0xf0000u)
    return CC_INVALID;
  if (cc_wave_validate(s, &samples))
    return CC_NOTPOSSIBLE;
  if (((flags & 0x20000) && d->channels != s->channels) ||
      ((flags & 0x40000) && d->rate != s->rate) ||
      ((flags & 0x80000) && d->bits != (s->tag == 1 ? s->bits : 16)) ||
      ((flags & 0x10000) && d->tag != 1))
    return CC_NOTPOSSIBLE;
  d->tag = 1;
  d->channels = s->channels;
  d->rate = s->rate;
  d->bits = s->tag == 1 ? s->bits : 16;
  d->align = (uint16_t)(s->channels * (d->bits / 8));
  d->average = d->rate * d->align;
  if (bytes >= 18)
    d->extra = 0;
  return 0;
}
uint32_t cc_stream_size(const struct cc_wave *s, uint32_t bytes, uint32_t flags,
                        uint32_t *out) {
  uint32_t samples, unit, blocks;
  if (!out || flags > 1)
    return CC_INVALID;
  if (cc_wave_validate(s, &samples))
    return CC_NOTPOSSIBLE;
  if (s->tag == 1) {
    *out = bytes - bytes % s->align;
    return 0;
  }
  unit = samples * s->channels * 2;
  if (flags == 0) {
    blocks = bytes / s->align;
    if (!blocks)
      return CC_NOTPOSSIBLE;
    if (bytes % s->align)
      blocks++;
  } else {
    blocks = bytes / unit;
    if (!blocks)
      return CC_NOTPOSSIBLE;
  }
  unit = flags ? s->align : unit;
  if (blocks > 0xffffffffu / unit)
    return CC_NOTPOSSIBLE;
  *out = blocks * unit;
  return 0;
}
static int ima(int code, int *sample, int *index) {
  int step = steps[*index], diff = step / 8;
  if (code & 1)
    diff += step / 4;
  if (code & 2)
    diff += step / 2;
  if (code & 4)
    diff += step;
  *sample = cc_clip(*sample + ((code & 8) ? -diff : diff), -32768, 32767);
  *index = cc_clip(*index + indices[code & 7], 0, 88);
  return *sample;
}
static int ms(int code, int *s1, int *s2, int *delta, int c1, int c2) {
  /* int64 avoids adversarial coefficient/delta multiplication overflow. */
  int64_t prediction = ((int64_t)*s1 * c1 + (int64_t)*s2 * c2) / 256;
  int64_t value = prediction + (code >= 8 ? code - 16 : code) * (int64_t)*delta;
  int result = value < -32768 ? -32768 : (value > 32767 ? 32767 : (int)value);
  int64_t next = (int64_t)adapt[code] * *delta / 256;
  *delta = next < 16 ? 16 : (next > 0x7fffffff ? 0x7fffffff : (int)next);
  *s2 = *s1;
  *s1 = result;
  return result;
}
static void put(uint8_t *d, uint32_t i, int s) {
  d[2 * i] = (uint8_t)s;
  d[2 * i + 1] = (uint8_t)((uint32_t)s >> 8);
}
uint32_t cc_audio(const struct cc_wave *f, const uint8_t *src, uint32_t len,
                  uint8_t *dst, uint32_t cap, uint32_t *used, uint32_t *written,
                  uint32_t flags) {
  uint32_t samples, ch, header, at = 0, out = 0;
  if (!used || !written)
    return CC_INVALID;
  *used = *written = 0;
  if ((len && !src) || (cap && !dst) || flags & ~0x34u)
    return CC_INVALID;
  if (cc_wave_validate(f, &samples))
    return CC_NOTPOSSIBLE;
  if (f->tag == 1) {
    uint32_t bytes = len < cap ? len : cap;
    bytes -= bytes % f->align;
    cc_copy(dst, src, bytes);
    *used = *written = bytes;
    return 0;
  }
  ch = f->channels;
  header = (f->tag == 17 ? 4 : 7) * ch;
  while (at < len) {
    uint32_t n = len - at, frames, c, j;
    int s1[2] = {0, 0}, s2[2] = {0, 0}, delta[2] = {0, 0}, coef1[2] = {0, 0},
        coef2[2] = {0, 0};
    const uint8_t *p = src + at;
    if (n > f->align)
      n = f->align;
    /* Without END preserve incomplete source for the next call. */
    if (n < f->align && (!(flags & 0x20u) || (flags & 4u)))
      break;
    if (n < header)
      return CC_NOTPOSSIBLE;
    frames = f->tag == 17 ? 1 + (n - header) / (4 * ch) * 8
                          : 2 + (n - header) * 2 / ch;
    if (f->tag == 17 && ch == 1)
      frames = 1 + (n - header) * 2;
    else if (f->tag == 17 && (n - header) % 8 > 4)
      frames += ((n - header) % 8 - 4) * 2;
    if (frames > samples)
      frames = samples;
    if (frames > (cap - out) / (2 * ch))
      break;
    for (c = 0; c < ch; c++) {
      if (f->tag == 17) {
        s1[c] = (int16_t)cc_le16(p + 4 * c);
        delta[c] = p[4 * c + 2];
        if (delta[c] > 88 || p[4 * c + 3])
          return CC_NOTPOSSIBLE;
        put(dst + out, c, s1[c]);
      } else {
        uint32_t count = cc_le16((const uint8_t *)f + 20), pred = p[c];
        const uint8_t *co = (const uint8_t *)f + 22 + 4 * pred;
        if (pred >= count)
          return CC_NOTPOSSIBLE;
        coef1[c] = (int16_t)cc_le16(co);
        coef2[c] = (int16_t)cc_le16(co + 2);
        delta[c] = (int16_t)cc_le16(p + ch + 2 * c);
        if (delta[c] < 16)
          return CC_NOTPOSSIBLE;
        s1[c] = (int16_t)cc_le16(p + 3 * ch + 2 * c);
        s2[c] = (int16_t)cc_le16(p + 5 * ch + 2 * c);
        put(dst + out, c, s2[c]);
        put(dst + out, ch + c, s1[c]);
      }
    }
    for (j = f->tag == 17 ? 1 : 2; j < frames; j++)
      for (c = 0; c < ch; c++) {
        uint32_t k = j - (f->tag == 17 ? 1 : 2), byte;
        int code, s;
        if (f->tag == 17) {
          byte = header + (k / 8) * (4 * ch) + 4 * c + (k % 8) / 2;
          code = (p[byte] >> ((k & 1) * 4)) & 15;
          s = ima(code, &s1[c], &delta[c]);
        } else {
          uint32_t nib = k * ch + c;
          byte = header + nib / 2;
          code = (p[byte] >> ((nib & 1) ? 0 : 4)) & 15;
          s = ms(code, &s1[c], &s2[c], &delta[c], coef1[c], coef2[c]);
        }
        put(dst + out, j * ch + c, s);
      }
    out += frames * ch * 2;
    at += n;
    *used = at;
    *written = out;
  }
  return 0;
}
