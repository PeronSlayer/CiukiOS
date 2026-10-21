/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "authored.h"
#include "codec.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern uint32_t test_live(void);
extern uintptr_t ICOpen(uint32_t, uint32_t, uint32_t);
extern intptr_t ICSendMessage(uintptr_t, uint32_t, uintptr_t, uintptr_t);
extern int32_t ICDecompress(uintptr_t, uint32_t, struct cc_bitmap *,
                            const void *, struct cc_bitmap *, void *);
extern int32_t ICClose(uintptr_t);
extern int32_t AVIFileOpenA(void **, const char *, uint32_t, void *);
extern uint32_t AVIFileRelease(void *);
extern int32_t AVIFileGetStream(void *, void **, uint32_t, int32_t);
extern uint32_t AVIStreamRelease(void *);
extern void *AVIStreamGetFrameOpen(void *, struct cc_bitmap *);
extern void *AVIStreamGetFrame(void *, int32_t);
extern int32_t AVIStreamGetFrameClose(void *);
extern int32_t AVIFileInfoA(void *, void *, int32_t);
extern void test_file(const uint8_t *, uint32_t);
extern uint32_t acmFormatSuggest(uintptr_t, const struct cc_wave *,
                                  struct cc_wave *, uint32_t, uint32_t);
static uint32_t rng = 0x1234abcd;
static uint32_t next(void) {
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;
  return rng;
}
static void ms_boundaries(void) {
  static const int16_t coef[7][2] = {{256,0},{512,-256},{0,0},{192,64},{240,0},{460,-208},{392,-232}};
  for (unsigned channels = 1; channels <= 2; channels++) {
    uint8_t format[54] = {0};
    struct cc_wave *w = (void *)format;
    w->tag = 2; w->channels = (uint16_t)channels; w->rate = 22050;
    w->bits = 4; w->align = (uint16_t)(7*channels); w->extra = 36;
    format[18] = 2; format[20] = 8;
    for (unsigned i = 0; i < 8; i++) for (unsigned j = 0; j < 2; j++) {
      uint16_t value = (uint16_t)coef[i%7][j];
      format[22+4*i+2*j] = (uint8_t)value;
      format[23+4*i+2*j] = (uint8_t)(value >> 8);
    }
    for (unsigned predictor = 0; predictor < 8; predictor++) {
      uint8_t header[14] = {0}, pcm[8]; uint32_t used, written;
      for (unsigned c = 0; c < channels; c++) {
        header[c] = (uint8_t)predictor;
        header[channels+2*c] = 0xff; header[channels+2*c+1] = 0x7f;
        header[3*channels+2*c] = 0xff; header[3*channels+2*c+1] = 0x7f;
        header[5*channels+2*c+1] = 0x80;
      }
      assert(!cc_audio(w,header,7*channels,pcm,sizeof(pcm),&used,&written,0x20));
      assert(used == 7*channels && written == 4*channels);
      for (unsigned c = 0; c < channels; c++) {
        assert(cc_le16(pcm+2*c) == 0x8000);
        assert(cc_le16(pcm+2*(channels+c)) == 0x7fff);
      }
    }
    for (unsigned i = 0; i < 28; i++) {
      uint32_t samples; format[22+i] ^= 1;
      assert(cc_wave_validate(w,&samples) == CC_NOTPOSSIBLE);
      format[22+i] ^= 1;
    }
    /* Maximum representable block/sample geometry and saturating samples. */
    w->align = (uint16_t)(32760*channels);
    unsigned samples = (w->align - 7*channels)*2/channels+2;
    format[18] = (uint8_t)samples; format[19] = (uint8_t)(samples >> 8);
    uint8_t *block = malloc(w->align), *pcm = malloc(samples*channels*2);
    assert(block && pcm); memset(block,0x77,w->align);
    for (unsigned c = 0; c < channels; c++) {
      block[c] = 0;
      block[channels+2*c] = 0xff; block[channels+2*c+1] = 0x7f;
      block[3*channels+2*c] = 0xff; block[3*channels+2*c+1] = 0x7f;
      block[5*channels+2*c+1] = 0x80;
    }
    uint32_t used, written;
    assert(!cc_audio(w,block,w->align,pcm,samples*channels*2,&used,&written,0x20));
    assert(used == w->align && written == samples*channels*2);
    for (unsigned i = 2*channels; i < samples*channels; i++) assert(cc_le16(pcm+2*i) == 0x7fff);
    memset(block+7*channels,0x88,w->align-7*channels);
    assert(!cc_audio(w,block,w->align,pcm,samples*channels*2,&used,&written,0x20));
    for (unsigned i = 2*channels; i < samples*channels; i++) assert(cc_le16(pcm+2*i) == 0x8000);
    free(block); free(pcm);
  }
}
int main(void) {
  ms_boundaries();
  /* PCMWAVEFORMAT can end exactly at byte 16 (no WAVEFORMATEX cbSize).
   * Heap redzones catch widening this public input to an 18-byte load. */
  uint8_t *pcm = malloc(16), *suggested = malloc(16);
  assert(pcm && suggested);
  const uint8_t format[16] = {1,0,1,0,0x40,0x1f,0,0,0x80,0x3e,0,0,2,0,16,0};
  memcpy(pcm, format, 16);
  assert(!acmFormatSuggest(0, (const void *)pcm, (void *)suggested, 16, 0));
  assert(!memcmp(suggested, format, 16));
  free(pcm);
  free(suggested);
  uint32_t strip = sizeof(seed2) - 10, size = 10 + strip * 2;
  uint8_t *multi = malloc(size), pixels[96];
  assert(multi);
  memcpy(multi, seed2, 10); memcpy(multi+10, seed2+10, strip);
  memcpy(multi+10+strip, seed2+10, strip);
  multi[1] = (uint8_t)(size >> 16); multi[2] = (uint8_t)(size >> 8); multi[3] = (uint8_t)size;
  multi[7] = 8; multi[9] = 2;
  struct cc_bitmap mi = {40,4,8,1,24,CC_FOUR('c','v','i','d'),size,0,0,0,0}, mo = mi;
  mo.compression = 0; mo.bytes = sizeof(pixels);
  uintptr_t mh = ICOpen(CC_FOUR('v','i','d','c'), mi.compression, 2);
  assert(mh && !ICSendMessage(mh,0x400c,(uintptr_t)&mi,(uintptr_t)&mo));
  assert(!ICDecompress(mh,0,&mi,multi,&mo,pixels));
  for (uint32_t n = 1; n < size; n++) {
    mi.bytes = n;
    assert(ICDecompress(mh,0,&mi,multi,&mo,pixels) < 0);
  }
  mi.bytes = size;
  for (unsigned field = 0; field < 12; field++) {
    unsigned at = 10 + strip + field;
    multi[at] ^= 0x80;
    assert(ICDecompress(mh,0,&mi,multi,&mo,pixels) < 0);
    multi[at] ^= 0x80;
  }
  assert(!ICClose(mh)); free(multi);
  struct cc_bitmap large = {40,2048,2048,1,16,CC_FOUR('m','s','v','c'),2,0,0,0,0}, ld = large;
  ld.compression = 0; ld.bytes = 2048u*2048u*2;
  uint8_t *lp = malloc(ld.bytes); assert(lp);
  mh = ICOpen(CC_FOUR('v','i','d','c'),large.compression,2);
  assert(mh && !ICSendMessage(mh,0x400c,(uintptr_t)&large,(uintptr_t)&ld));
  assert(ICDecompress(mh,0,&large,seed1,&ld,lp) < 0);
  large.width = 2049;
  assert(ICSendMessage(mh,0x400b,(uintptr_t)&large,(uintptr_t)&ld) < 0);
  assert(!ICClose(mh)); free(lp);
  uint32_t iteration,
      kinds[] = {CC_FOUR('m', 'r', 'l', 'e'), CC_FOUR('m', 's', 'v', 'c'),
                 CC_FOUR('c', 'v', 'i', 'd')};
  for (iteration = 0; iteration < 24000; iteration++) {
    uint32_t n = next() % 193, i, kind = kinds[iteration % 3];
    const uint8_t *seed =
        iteration % 3 == 0 ? seed0 : (iteration % 3 == 1 ? seed1 : seed2);
    uint32_t seedlen =
        iteration % 3 == 0
            ? sizeof(seed0)
            : (iteration % 3 == 1 ? sizeof(seed1) : sizeof(seed2));
    if (iteration % 5 < 3)
      n = iteration % 5 == 0
              ? seedlen
              : (iteration % 5 == 1 ? next() % seedlen : seedlen + next() % 17);
    uint8_t *s = malloc(n ? n : 1), out[96], wave[50];
    struct cc_bitmap in = {40, 4, 4, 1, 8, 1, n, 0, 0, 0, 0}, dest = in;
    uintptr_t h;
    for (i = 0; i < n; i++)
      s[i] = i < seedlen ? seed[i] : (uint8_t)next();
    if (n && iteration % 4 == 0)
      s[next() % n] ^= (uint8_t)(1u << (next() % 8));
    memset(out, 0xa5, sizeof(out));
    in.compression = kind == kinds[0] ? (iteration & 1 ? 1 : 2) : kind;
    in.bits = kind == kinds[0]
                  ? (in.compression == 1 ? 8 : 4)
                  : (kind == kinds[1] ? (iteration & 1 ? 8 : 16) : 24);
    dest.bits = kind == kinds[0] ? 8 : in.bits;
    dest.compression = 0;
    dest.bytes = dest.bits == 24 ? 48 : (dest.bits == 16 ? 32 : 16);
    h = ICOpen(CC_FOUR('v', 'i', 'd', 'c'), kind, 2);
    assert(h);
    assert(!ICSendMessage(h, 0x400c, (uintptr_t)&in, (uintptr_t)&dest));
    if (n) {
      int32_t result = ICDecompress(h, 0, &in, s, &dest, out + 16);
      assert(result == 0 || result < 0);
    }
    for (i = 0; i < 16; i++)
      assert(out[i] == 0xa5);
    for (i = 16 + dest.bytes; i < 96; i++)
      assert(out[i] == 0xa5);
    assert(!ICClose(h));
    memset(wave, 0, sizeof(wave));
    struct cc_wave *w = (void *)wave;
    w->tag = iteration & 1 ? 17 : 2;
    w->channels = iteration & 2 ? 2 : 1;
    w->rate = 22050;
    w->bits = 4;
    w->align = (uint16_t)(w->tag == 17 ? 8 * w->channels : 8 * w->channels);
    w->extra = w->tag == 17 ? 2 : 32;
    wave[18] = (uint8_t)((w->align - (w->tag == 17 ? 4 : 7) * w->channels) * 2 /
                             w->channels +
                         (w->tag == 17 ? 1 : 2));
    wave[20] = 7;
    static const int16_t coef[7][2] = {{256,0},{512,-256},{0,0},{192,64},{240,0},{460,-208},{392,-232}};
    for (unsigned c = 0; c < 7; c++) for (unsigned j = 0; j < 2; j++) {
      unsigned v = (uint16_t)coef[c][j];
      wave[22 + 4*c + 2*j] = (uint8_t)v;
      wave[23 + 4*c + 2*j] = (uint8_t)(v >> 8);
    }
    if (w->tag == 2 && iteration % 11 == 0) wave[22 + next()%28] ^= 1;
    if (n >= (w->tag == 17 ? 4u : 7u) * w->channels && iteration % 3) {
      for (i = 0; i < w->channels; i++) {
        if (w->tag == 17) {
          s[4 * i + 2] %= 89;
          s[4 * i + 3] = 0;
        } else {
          s[i] = (uint8_t)((iteration / 2 + i) % 7);
          s[w->channels + 2 * i] = 32;
          s[w->channels + 2 * i + 1] = 0;
        }
      }
    }
    uint32_t used, written;
    cc_audio(w, s, n, out + 16, 64, &used, &written, 0x20);
    assert(used <= n && written <= 64);
    uint32_t an =
        iteration % 3 == 0 ? sizeof(seed3) : (next() % (sizeof(seed3) + 32));
    uint8_t *avi = malloc(an ? an : 1);
    for (i = 0; i < an; i++)
      avi[i] = i < sizeof(seed3) ? seed3[i] : (uint8_t)next();
    if (an && iteration % 4)
      avi[next() % an] ^= (uint8_t)(1u << (next() % 8));
    if (an >= 8 && iteration % 2) {
      uint32_t size = an - 8;
      memcpy(avi + 4, &size, 4);
    }
    test_file(avi, an);
    void *file = 0;
    if (!AVIFileOpenA(&file, "authored", 0, 0)) {
      unsigned char info[108];
      AVIFileInfoA(file, info, sizeof(info));
      void *stream = 0;
      if (!AVIFileGetStream(file, &stream, CC_FOUR('v', 'i', 'd', 's'), 0)) {
        struct cc_bitmap wanted = {40, 4, 4, 1, 8, 0, 0, 0, 0, 0, 0};
        void *get = AVIStreamGetFrameOpen(stream, iteration & 1 ? &wanted : 0);
        if (get) {
          AVIStreamGetFrame(get, 0);
          AVIStreamGetFrame(get, 1);
          AVIStreamGetFrameClose(get);
        }
        AVIStreamRelease(stream);
      }
      AVIFileRelease(file);
    }
    free(avi);
    assert(test_live() == 0);
    free(s);
  }
  puts("PASS 24000 deterministic malformed frames/audio/RIFF under ASan+UBSan");
  return 0;
}
