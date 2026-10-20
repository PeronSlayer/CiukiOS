/* SPDX-License-Identifier: LGPL-2.1-or-later
 * ACM PCM converter (f5-12B): 8/16-bit, mono/stereo and sample-rate
 * conversion between WAVE_FORMAT_PCM formats. 8-bit PCM is unsigned with a
 * 128 bias, 16-bit is signed little endian (Multimedia Standards Update,
 * WAVE data format). Rate conversion repeats/drops whole frames: output
 * frame n takes source frame floor(n * src_rate / dst_rate), tracked as an
 * integer index plus a remainder in 1/dst_rate units that persist across
 * conversions of one stream (acmStreamReset clears them). Stereo to mono
 * averages both channels. 32-bit arithmetic only (no compiler runtime in
 * the codec images). */
#include "codec.h"
int cc_pcm_supported(const struct cc_wave *s, const struct cc_wave *d) {
  return s && d && s->tag == 1 && d->tag == 1 &&
         (s->channels == 1 || s->channels == 2) &&
         (d->channels == 1 || d->channels == 2) &&
         (s->bits == 8 || s->bits == 16) && (d->bits == 8 || d->bits == 16) &&
         s->rate && d->rate && s->rate <= 192000 && d->rate <= 192000 &&
         s->align == s->channels * (s->bits / 8) &&
         d->align == d->channels * (d->bits / 8);
}
static int sample(const struct cc_wave *f, const uint8_t *p, uint32_t ch) {
  if (f->bits == 8)
    return ((int)p[ch] - 128) * 256;
  return (int16_t)cc_le16(p + 2 * ch);
}
static void store(const struct cc_wave *f, uint8_t *p, uint32_t ch, int v) {
  if (f->bits == 8)
    p[ch] = (uint8_t)((v >> 8) + 128);
  else {
    p[2 * ch] = (uint8_t)v;
    p[2 * ch + 1] = (uint8_t)((uint32_t)v >> 8);
  }
}
/* 64-by-32 unsigned division by shift and subtract. */
static uint64_t div64(uint64_t n, uint32_t d, uint32_t *remainder) {
  uint64_t q = 0, r = 0;
  for (int bit = 63; bit >= 0; bit--) {
    r = (r << 1) | ((n >> bit) & 1);
    if (r >= d) {
      r -= d;
      q |= (uint64_t)1 << bit;
    }
  }
  if (remainder)
    *remainder = (uint32_t)r;
  return q;
}
/* Converts whole source frames. *phase holds the index of the next source
 * frame relative to the first unconsumed one (high word) and the remainder
 * in 1/dst_rate units (low word). */
uint32_t cc_pcm_convert(const struct cc_wave *s, const struct cc_wave *d,
                        uint64_t *phase, const uint8_t *src, uint32_t len,
                        uint8_t *dst, uint32_t cap, uint32_t *used,
                        uint32_t *written) {
  if (!used || !written || (len && !src) || (cap && !dst) ||
      !cc_pcm_supported(s, d))
    return CC_INVALID;
  uint32_t frames = len / s->align, out = 0, max = cap / d->align;
  uint32_t index = (uint32_t)(*phase >> 32), rest = (uint32_t)*phase;
  uint32_t whole = s->rate / d->rate, part = s->rate % d->rate;
  while (out < max && index < frames) {
    const uint8_t *p = src + index * s->align;
    int l = sample(s, p, 0), r = s->channels == 2 ? sample(s, p, 1) : l;
    uint8_t *q = dst + out * d->align;
    if (d->channels == 1)
      store(d, q, 0, (l + r) / 2);
    else {
      store(d, q, 0, l);
      store(d, q, 1, r);
    }
    out++;
    index += whole;
    rest += part;
    if (rest >= d->rate) {
      rest -= d->rate;
      index++;
    }
  }
  uint32_t consumed = index > frames ? frames : index;
  *phase = (uint64_t)(index - consumed) << 32 | rest;
  *used = consumed * s->align;
  *written = out * d->align;
  return 0;
}
/* Byte sizes for acmStreamSize: flags 0 = source bytes -> destination,
 * 1 = destination bytes -> source. */
uint32_t cc_pcm_size(const struct cc_wave *s, const struct cc_wave *d,
                     uint32_t bytes, uint32_t flags, uint32_t *out) {
  uint32_t frames;
  uint64_t n;
  if (!out || flags > 1 || !cc_pcm_supported(s, d))
    return CC_INVALID;
  if (!flags) {
    frames = bytes / s->align;
    n = div64((uint64_t)frames * d->rate + s->rate - 1, s->rate, 0) * d->align;
  } else {
    frames = bytes / d->align;
    n = div64((uint64_t)frames * s->rate + d->rate - 1, d->rate, 0) * s->align;
  }
  if (!frames || n > 0xffffffffu)
    return CC_NOTPOSSIBLE;
  *out = (uint32_t)n;
  return 0;
}
