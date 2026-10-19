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
static uint32_t rng = 0x1234abcd;
static uint32_t next(void) {
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;
  return rng;
}
int main(void) {
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
    wave[23] = 1;
    if (n >= (w->tag == 17 ? 4u : 7u) * w->channels && iteration % 2) {
      for (i = 0; i < w->channels; i++) {
        if (w->tag == 17) {
          s[4 * i + 2] %= 89;
          s[4 * i + 3] = 0;
        } else {
          s[i] %= 7;
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
