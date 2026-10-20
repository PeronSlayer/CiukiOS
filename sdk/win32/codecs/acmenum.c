/* SPDX-License-Identifier: LGPL-2.1-or-later
 * msacm32 discovery (f5-12B): acmDriverDetails, acmFormatTagDetails/Enum,
 * acmFormatDetails/Enum for the three installed drivers (PCM converter 1,
 * Microsoft ADPCM 2, IMA ADPCM 17; the driver ids equal their tags).
 * Structures: Platform SDK ACMDRIVERDETAILS, ACMFORMATTAGDETAILS,
 * ACMFORMATDETAILS. Standard formats: 8, 11.025, 22.05 and 44.1 kHz, mono
 * and stereo; PCM at 8 and 16 bits; ADPCM blocks of 256 bytes per channel
 * per 11.025 kHz step (Multimedia Standards Update rev. 3.0 examples). */
#include "codec.h"
#ifndef W32_HOST_MODEL
_Static_assert(sizeof(void *) == 4, "i386 layouts below");
#endif
struct driver_details {
  uint32_t size, type, comp;
  uint16_t mid, pid;
  uint32_t acm, version, support, tags, filters;
  uintptr_t icon;
  char text[880]; /* short 32, long 128, copyright 80, licensing 128,
                     features 512; W: twice */
};
struct tag_details {
  uint32_t size, index, tag, format_size, support, standard;
  char name[96]; /* 48 characters */
};
struct format_details {
  uint32_t size, index, tag, support;
  struct cc_wave *format;
  uint32_t format_size;
  char name[256]; /* 128 characters */
};
#ifndef W32_HOST_MODEL
_Static_assert(sizeof(struct driver_details) - 880 == 40, "ACMDRIVERDETAILS");
_Static_assert(__builtin_offsetof(struct tag_details, name) == 24,
               "ACMFORMATTAGDETAILS");
_Static_assert(__builtin_offsetof(struct format_details, name) == 24,
               "ACMFORMATDETAILS");
#endif
typedef int(W32API *tag_callback)(uintptr_t, struct tag_details *, uintptr_t,
                                  uint32_t);
typedef int(W32API *format_callback)(uintptr_t, struct format_details *,
                                     uintptr_t, uint32_t);
static const uint32_t rates[4] = {8000, 11025, 22050, 44100};
static const int16_t coefficients[7][2] = {{256, 0},   {512, -256}, {0, 0},
                                           {192, 64},  {240, 0},    {460, -208},
                                           {392, -232}};
static int valid_driver(uintptr_t h) { return h == 1 || h == 2 || h == 17; }
static void put_text(char *out, uint32_t chars, const char *s, int wide) {
  uint32_t i = 0;
  for (; s[i] && i + 1 < chars; i++)
    if (wide) {
      out[2 * i] = s[i];
      out[2 * i + 1] = 0;
    } else
      out[i] = s[i];
  if (wide) {
    out[2 * i] = 0;
    out[2 * i + 1] = 0;
  } else
    out[i] = 0;
}
static uint32_t details(uintptr_t h, struct driver_details *d, uint32_t flags,
                        int wide) {
  static const uint32_t fields[5] = {32, 128, 80, 128, 512};
  if (!valid_driver(h))
    return 5;
  if (!d || d->size < 4)
    return CC_INVALID;
  if (flags)
    return 10;
  uint32_t full = 40 + 880 * (wide ? 2 : 1);
  uint32_t size = d->size < full ? d->size : full;
  uint8_t record[40 + 1760];
  cc_zero(record, sizeof(record));
  uint32_t head[11] = {size,
                       CC_FOUR('a', 'u', 'd', 'c'),
                       0,
                       1u | (h == 1 ? 38u : h == 2 ? 33u : 34u) << 16,
                       0x03320000u,
                       0x04000000u,
                       h == 1 ? 2u : 1u,
                       h == 1 ? 1u : 2u,
                       0,
                       0,
                       0};
  cc_copy(record, head, 40);
  const char *texts[5] = {
      h == 1 ? "MS-PCM" : h == 2 ? "MS-ADPCM" : "IMA ADPCM",
      h == 1   ? "CiukiOS PCM converter"
      : h == 2 ? "CiukiOS Microsoft ADPCM decoder"
               : "CiukiOS IMA ADPCM decoder",
      "Copyright 2026 CiukiOS contributors", "LGPL-2.1-or-later",
      h == 1 ? "Converts PCM sample size, channels and rate."
             : "Decodes to 16-bit PCM; no encoder."};
  uint32_t at = 40;
  for (unsigned i = 0; i < 5; i++) {
    put_text((char *)record + at, fields[i], texts[i], wide);
    at += fields[i] * (wide ? 2 : 1);
  }
  cc_copy(d, record, size);
  d->size = size;
  return 0;
}
uint32_t W32API acmDriverDetailsA(uintptr_t h, struct driver_details *d,
                                  uint32_t flags) {
  return details(h, d, flags, 0);
}
uint32_t W32API acmDriverDetailsW(uintptr_t h, struct driver_details *d,
                                  uint32_t flags) {
  return details(h, d, flags, 1);
}

/* Format tags per driver: the PCM converter has PCM; each codec has PCM
 * and its own tag. */
static uint32_t tags_of(uintptr_t h, uint32_t out[2]) {
  out[0] = 1;
  if (h == 1)
    return 1;
  out[1] = (uint32_t)h;
  return 2;
}
static uint32_t format_size(uint32_t tag) {
  return tag == 2 ? 50 : tag == 17 ? 20 : 16;
}
static uint32_t standard_count(uint32_t tag) { return tag == 1 ? 16 : 8; }
static void fill_tag(uintptr_t h, uint32_t index, uint32_t tag,
                     struct tag_details *t, int wide) {
  t->index = index;
  t->tag = tag;
  t->format_size = format_size(tag);
  t->support = h == 1 ? 2 : 1; /* CONVERTER / CODEC */
  t->standard = standard_count(tag);
  put_text(t->name, 48,
           tag == 1 ? "PCM" : tag == 2 ? "Microsoft ADPCM" : "IMA ADPCM",
           wide);
}
static uint32_t tag_details(uintptr_t h, struct tag_details *t,
                            uint32_t flags, int wide) {
  uint32_t full = wide ? 120 : 72, list[2], n;
  if (h && !valid_driver(h))
    return 5;
  if (!t || t->size < full)
    return CC_INVALID;
  if (flags & ~15u)
    return 10;
  uint32_t query = flags & 15;
  if (query == 0) { /* ACM_FORMATTAGDETAILSF_INDEX */
    if (!h)
      return CC_INVALID;
    n = tags_of(h, list);
    if (t->index >= n)
      return CC_NOTPOSSIBLE;
    fill_tag(h, t->index, list[t->index], t, wide);
  } else if (query == 1 || query == 2) { /* FORMATTAG / LARGESTSIZE */
    uint32_t want = t->tag, best = 0;
    uintptr_t owner = 0;
    static const uintptr_t drivers[3] = {17, 2, 1};
    for (unsigned d = 0; d < 3; d++) {
      if (h && drivers[d] != h)
        continue;
      n = tags_of(drivers[d], list);
      for (uint32_t i = 0; i < n; i++)
        if ((query == 1 && list[i] == want) ||
            (query == 2 && (!want || list[i] == want) &&
             format_size(list[i]) > format_size(best))) {
          if (query == 1 && owner)
            continue;
          best = list[i];
          owner = drivers[d];
        }
    }
    if (!owner)
      return CC_NOTPOSSIBLE;
    n = tags_of(owner, list);
    fill_tag(owner, best == list[0] ? 0 : 1, best, t, wide);
  } else
    return 10;
  t->size = full;
  return 0;
}
uint32_t W32API acmFormatTagDetailsA(uintptr_t h, struct tag_details *t,
                                     uint32_t f) {
  return tag_details(h, t, f, 0);
}
uint32_t W32API acmFormatTagDetailsW(uintptr_t h, struct tag_details *t,
                                     uint32_t f) {
  return tag_details(h, t, f, 1);
}
static uint32_t tag_enum(uintptr_t h, struct tag_details *t, tag_callback fn,
                         uintptr_t user, uint32_t flags, int wide) {
  static const uintptr_t drivers[3] = {17, 2, 1};
  uint32_t full = wide ? 120 : 72, list[2];
  if (h && !valid_driver(h))
    return 5;
  if (!t || t->size < full || !fn)
    return CC_INVALID;
  if (flags)
    return 10;
  for (unsigned d = 0; d < 3; d++) {
    if (h && drivers[d] != h)
      continue;
    uint32_t n = tags_of(drivers[d], list);
    for (uint32_t i = 0; i < n; i++) {
      /* All drivers: PCM once, from the PCM converter. */
      if (!h && list[i] == 1 && drivers[d] != 1)
        continue;
      t->size = full;
      fill_tag(drivers[d], i, list[i], t, wide);
      if (!fn(drivers[d], t, user, t->support))
        return 0;
    }
  }
  return 0;
}
uint32_t W32API acmFormatTagEnumA(uintptr_t h, struct tag_details *t,
                                  tag_callback fn, uintptr_t user,
                                  uint32_t flags) {
  return tag_enum(h, t, fn, user, flags, 0);
}
uint32_t W32API acmFormatTagEnumW(uintptr_t h, struct tag_details *t,
                                  tag_callback fn, uintptr_t user,
                                  uint32_t flags) {
  return tag_enum(h, t, fn, user, flags, 1);
}

/* Standard format `index` of `tag` into a WAVEFORMATEX buffer. */
static uint32_t standard(uint32_t tag, uint32_t index, uint8_t out[50]) {
  struct cc_wave w = {0};
  if (index >= standard_count(tag))
    return 0;
  uint32_t rate = rates[index % 4];
  w.tag = (uint16_t)tag;
  if (tag == 1) {
    w.bits = index & 4 ? 16 : 8;
    w.channels = index & 8 ? 2 : 1;
    w.rate = rate;
    w.align = (uint16_t)(w.channels * w.bits / 8);
    w.average = rate * w.align;
    cc_zero(out, 50);
    cc_copy(out, &w, 18);
    return 16;
  }
  uint32_t ch = index & 4 ? 2 : 1, step = rate / 11025 ? rate / 11025 : 1;
  uint32_t align = 256 * ch * step;
  uint32_t samples = tag == 17 ? (align - 4 * ch) * 2 / ch + 1
                               : (align - 7 * ch) * 2 / ch + 2;
  w.channels = (uint16_t)ch;
  w.rate = rate;
  w.align = (uint16_t)align;
  w.bits = 4;
  w.average = rate * align / samples; /* below 2^32: 44100 * 2048 */
  w.extra = tag == 17 ? 2 : 32;
  cc_zero(out, 50);
  cc_copy(out, &w, 18);
  out[18] = (uint8_t)samples;
  out[19] = (uint8_t)(samples >> 8);
  if (tag == 2) {
    out[20] = 7;
    for (unsigned i = 0; i < 7; i++) {
      out[22 + 4 * i] = (uint8_t)coefficients[i][0];
      out[23 + 4 * i] = (uint8_t)((uint16_t)coefficients[i][0] >> 8);
      out[24 + 4 * i] = (uint8_t)coefficients[i][1];
      out[25 + 4 * i] = (uint8_t)((uint16_t)coefficients[i][1] >> 8);
    }
  }
  return 18 + w.extra;
}
static void describe(const struct cc_wave *w, char *out, int wide) {
  char text[64];
  uint32_t at = 0, khz = w->rate / 1000, frac = w->rate % 1000;
  char digits[12];
  uint32_t n = 0;
  do {
    digits[n++] = (char)('0' + khz % 10);
    khz /= 10;
  } while (khz);
  while (n)
    text[at++] = digits[--n];
  text[at++] = '.';
  text[at++] = (char)('0' + frac / 100);
  text[at++] = (char)('0' + frac / 10 % 10);
  text[at++] = (char)('0' + frac % 10);
  const char *tail = " kHz, ";
  while (*tail)
    text[at++] = *tail++;
  if (w->bits >= 10)
    text[at++] = (char)('0' + w->bits / 10);
  text[at++] = (char)('0' + w->bits % 10);
  tail = w->channels == 2 ? " Bit, Stereo" : " Bit, Mono";
  while (*tail)
    text[at++] = *tail++;
  text[at] = 0;
  put_text(out, 128, text, wide);
}
static uint32_t format_details(uintptr_t h, struct format_details *f,
                               uint32_t flags, int wide) {
  uint32_t full = wide ? 280 : 152, list[2];
  if (h && !valid_driver(h))
    return 5;
  if (!f || f->size < full || !f->format || f->format_size < 16)
    return CC_INVALID;
  if (flags & ~15u)
    return 10;
  if ((flags & 15) == 0) { /* ACM_FORMATDETAILSF_INDEX */
    uint8_t w[50];
    uint32_t n = h ? tags_of(h, list) : 0, ok = 0;
    for (uint32_t i = 0; i < n; i++)
      ok |= list[i] == f->tag;
    if (h && !ok)
      return CC_NOTPOSSIBLE;
    uint32_t size = standard(f->tag, f->index, w);
    if (!size)
      return CC_NOTPOSSIBLE;
    if (f->format_size < size)
      return CC_INVALID;
    cc_copy(f->format, w, size);
  } else if ((flags & 15) == 1) { /* ACM_FORMATDETAILSF_FORMAT */
    uint32_t samples;
    if (f->format->tag != f->tag && f->tag)
      return CC_INVALID;
    if (cc_wave_validate(f->format, &samples))
      return CC_NOTPOSSIBLE;
    if (h && f->format->tag != 1 && f->format->tag != h)
      return CC_NOTPOSSIBLE;
    f->tag = f->format->tag;
  } else
    return 10;
  f->support = h == 1 || (!h && f->tag == 1) ? 2 : 1;
  describe(f->format, f->name, wide);
  return 0;
}
uint32_t W32API acmFormatDetailsA(uintptr_t h, struct format_details *f,
                                  uint32_t flags) {
  return format_details(h, f, flags, 0);
}
uint32_t W32API acmFormatDetailsW(uintptr_t h, struct format_details *f,
                                  uint32_t flags) {
  return format_details(h, f, flags, 1);
}
/* ACM_FORMATENUMF_* restrict the standard list; CONVERT/SUGGEST list the
 * destinations reachable from *pwfx (decode to PCM16 for ADPCM, the PCM
 * converter for PCM); HARDWARE lists nothing (no hardware codecs). */
static uint32_t format_enum(uintptr_t h, struct format_details *f,
                            format_callback fn, uintptr_t user,
                            uint32_t flags, int wide) {
  static const uintptr_t drivers[3] = {17, 2, 1};
  uint32_t list[2];
  if (h && !valid_driver(h))
    return 5;
  if (!f || f->size < (wide ? 280u : 152u) || !f->format ||
      f->format_size < 16 || !fn)
    return CC_INVALID;
  if (flags & ~0x1ff0000u)
    return 10;
  if ((flags & 0x300000) == 0x300000)
    return 10;
  struct cc_wave in;
  cc_copy(&in, f->format, 16);
  in.extra = 0;
  uint32_t source_tag = 0, samples;
  if (flags & 0x300000) { /* CONVERT / SUGGEST */
    cc_copy(&in, f->format, f->format_size >= 18 ? 18 : 16);
    if (cc_wave_validate(f->format, &samples))
      return CC_NOTPOSSIBLE;
    source_tag = f->format->tag;
  }
  if (flags & 0x400000)
    return 0;
  for (unsigned d = 0; d < 3; d++) {
    if (h && drivers[d] != h)
      continue;
    uint32_t n = tags_of(drivers[d], list);
    for (uint32_t t = 0; t < n; t++) {
      uint32_t tag = list[t];
      if (!h && tag == 1 && drivers[d] != 1)
        continue;
      if ((flags & 0x10000) && tag != in.tag)
        continue;
      if (f->tag && f->tag != tag && !(flags & 0x10000))
        continue;
      for (uint32_t i = 0; i < standard_count(tag); i++) {
        uint8_t w[50];
        uint32_t size = standard(tag, i, w);
        const struct cc_wave *s = (const void *)w;
        if (((flags & 0x20000) && s->channels != in.channels) ||
            ((flags & 0x40000) && s->rate != in.rate) ||
            ((flags & 0x80000) && s->bits != in.bits))
          continue;
        if (source_tag) {
          /* Only PCM destinations exist; ADPCM decodes at its own rate,
           * channel count and 16 bits; SUGGEST keeps the source layout. */
          if (tag != 1)
            continue;
          if (source_tag != 1 &&
              (s->bits != 16 || s->channels != in.channels ||
               s->rate != in.rate))
            continue;
          if (source_tag == 1 && (flags & 0x200000) &&
              (s->channels != in.channels || s->rate != in.rate))
            continue;
        }
        if (f->format_size < size)
          return CC_INVALID;
        cc_copy(f->format, w, size);
        f->index = i;
        f->tag = tag;
        f->support = drivers[d] == 1 ? 2 : 1;
        describe(s, f->name, wide);
        if (!fn(drivers[d], f, user, f->support))
          return 0;
      }
    }
  }
  return 0;
}
uint32_t W32API acmFormatEnumA(uintptr_t h, struct format_details *f,
                               format_callback fn, uintptr_t user,
                               uint32_t flags) {
  return format_enum(h, f, fn, user, flags, 0);
}
uint32_t W32API acmFormatEnumW(uintptr_t h, struct format_details *f,
                               format_callback fn, uintptr_t user,
                               uint32_t flags) {
  return format_enum(h, f, fn, user, flags, 1);
}
