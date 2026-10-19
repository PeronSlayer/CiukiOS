/* TinySoundFont integration. One instance, bounded metadata, shared S16
 * samples. LGPL-2.1-or-later; vendor renderer retains its MIT licence. */
#include "audio.h"
static void *resize(void *p, uint32_t n) {
  if (!p)
    return wa_alloc(n);
  uint32_t old = ((uint32_t *)p)[-4];
  void *q = wa_alloc(n);
  if (q) {
    w32_copy(q, p, n < old ? n : old);
    wa_free(p);
  }
  return q;
}
#ifndef W32_HOST_MODEL
/* x87 libm primitives; the i686 baseline has x87, never SSE/MMX. */
static double logarithm(double x) {
  double r;
  __asm__("fldln2; fldl %1; fyl2x" : "=t"(r) : "m"(x) : "st(1)");
  return r;
}
static double power(double x, double y) {
  double v = y * logarithm(x) * 1.4426950408889634074, r;
  __asm__("fldl %1; fld %%st(0); frndint; fxch; fsub %%st(1),%%st; f2xm1; "
          "fld1; faddp; fscale; fstp %%st(1)"
          : "=t"(r)
          : "m"(v)
          : "st(1)");
  return r;
}
static float root(float x) {
  float r;
  __asm__("fsqrt" : "=t"(r) : "0"(x));
  return r;
}
static double tangent(double x) {
  double r;
  __asm__("fptan; fstp %%st(0)" : "=t"(r) : "0"(x));
  return r;
}
#define TSF_POW power
#define TSF_POWF(x, y) ((float)power((x), (y)))
#define TSF_EXPF(x) ((float)power(2.7182818284590452354, (x)))
#define TSF_LOG logarithm
#define TSF_TAN tangent
#define TSF_LOG10(x) (logarithm(x) * 0.43429448190325182765)
#define TSF_SQRT root
#define TSF_SQRTF root
#endif
#define TSF_NO_STDIO
#define TSF_MALLOC wa_alloc
#define TSF_FREE wa_free
#define TSF_REALLOC resize
#define TSF_MEMCPY(d, s, n) w32_copy((d), (s), (uint32_t)(n))
#define TSF_MEMSET(d, v, n) w32_fill((d), (v), (uint32_t)(n))
#define TSF_CIUKI_S16
#define TSF_RENDER_EFFECTSAMPLEBLOCK 1
#define TSF_IMPLEMENTATION
#include "tsf/tsf.h"
static tsf *synth;
static uint32_t program[16], modulation[16], bend[16], phase, rate = 44100;
static uint32_t volume = 0xffffffffu, master = 16383;
uint32_t wa_bank_full(uint32_t installed_mib, uint32_t available_mib) {
  return installed_mib >= 384 && available_mib >= 224;
}
uint32_t wa_synth_load(const void *data, uint32_t bytes) {
  if (synth)
    return 0;
  if (bytes > 192 * 1024 * 1024 || bytes < 12)
    return 11;
  synth = tsf_load_memory(data, (int)bytes);
  if (!synth)
    return 7;
  tsf_set_output(synth, TSF_STEREO_INTERLEAVED, (int)rate, -6);
  if (!tsf_set_max_voices(synth, 64)) {
    tsf_close(synth);
    synth = 0;
    return 7;
  }
  for (unsigned c = 0; c < 16; c++) {
    bend[c] = 8192;
    tsf_channel_set_presetnumber(synth, c, 0, c == 9);
  }
  return 0;
}
void wa_synth_reset(void) {
  if (!synth)
    return;
  tsf_reset(synth);
  for (int i = 0; i < synth->voiceNum; i++)
    tsf_voice_kill(&synth->voices[i]);
  phase = 0;
  master = 16383;
  for (unsigned c = 0; c < 16; c++) {
    program[c] = modulation[c] = 0;
    bend[c] = 8192;
    tsf_channel_set_presetnumber(synth, c, 0, c == 9);
  }
}
uint32_t wa_synth_short(uint32_t msg) {
  uint32_t status = msg & 255, c = status & 15, a = (msg >> 8) & 127,
           b = (msg >> 16) & 127;
  if (status < 0x80 || status >= 0xf0)
    return 11;
  if (!synth)
    return 6;
  switch (status >> 4) {
  case 8:
    tsf_channel_note_off(synth, c, a);
    break;
  case 9:
    tsf_channel_note_on(synth, c, a, b / 127.0f);
    break;
  case 10:
  case 13:
    break;
  case 11:
    if (a == 1)
      modulation[c] = b;
    else
      tsf_channel_midi_control(synth, c, a, b);
    if (a == 121) {
      modulation[c] = 0;
      bend[c] = 8192;
      tsf_channel_set_pitchwheel(synth, c, 8192);
      tsf_channel_set_sustain(synth, c, 0);
    }
    break;
  case 12:
    program[c] = a;
    tsf_channel_set_presetnumber(synth, c, a, c == 9);
    break;
  case 14:
    bend[c] = a | (b << 7);
    tsf_channel_set_pitchwheel(synth, c, bend[c]);
    break;
  default:
    return 11;
  }
  return 0;
}
uint32_t wa_synth_long(const uint8_t *p, uint32_t n) {
  if (!p || n < 6 || p[0] != 0xf0 || p[n - 1] != 0xf7)
    return 11;
  if ((p[1] == 0x7e || p[1] == 0x7f) && p[3] == 9 && p[4] == 1 && n == 6) {
    wa_synth_reset();
    return 0;
  }
  if (p[1] == 0x7f && p[3] == 4 && p[4] == 1 && n == 8 && p[5] < 128 &&
      p[6] < 128) {
    master = p[5] | p[6] << 7;
    return 0;
  }
  return 8;
}
void wa_synth_volume(uint32_t v) { volume = v; }
uint32_t wa_synth_voices(void) {
  return synth ? (uint32_t)tsf_active_voice_count(synth) : 0;
}
void wa_synth_render(void *context, int32_t *sum, uint32_t n,
                     uint32_t output_rate) {
  (void)context;
  if (!synth)
    return;
  if (rate != output_rate) {
    rate = output_rate;
    tsf_set_output(synth, TSF_STEREO_INTERLEAVED, rate, -6);
  }
  for (unsigned i = 0; i < n; i++) { /* 5 Hz triangle modulation; bends/RPN
                                        range still applied by TSF. */
    if (!(phase % 64))
      for (unsigned c = 0; c < 16; c++)
        if (modulation[c]) {
          uint32_t period = rate / 5, pos = phase % period;
          int32_t triangle =
              pos < period / 2 ? (int32_t)(pos * 32768 / (period / 2)) - 16384
                               : 49152 - (int32_t)(pos * 32768 / (period / 2));
          int32_t wheel =
              (int32_t)bend[c] + triangle * (int32_t)modulation[c] / (127 * 32);
          tsf_channel_set_pitchwheel(synth, c,
                                     wheel < 0       ? 0
                                     : wheel > 16383 ? 16383
                                                     : wheel);
        }
    float f[2];
    tsf_render_float(synth, f, 1, 0);
    for (unsigned c = 0; c < 2; c++) {
      float v = f[c] * (float)((volume >> (16 * c)) & 65535) / 65535.0f *
                (float)master / 16383.0f;
      int32_t sample = v >= 1    ? 32767
                       : v <= -1 ? -32768
                                 : (int32_t)(v * 32767.5f);
      sum[i * 2 + c] += sample;
    }
    phase++;
  }
}
