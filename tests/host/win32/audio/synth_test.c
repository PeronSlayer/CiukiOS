#define _POSIX_C_SOURCE 200809L
#include "../../../../sdk/win32/audio/synth.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static uint32_t bytes, peak;
void *wa_alloc(uint32_t n) {
  uint32_t *p = calloc(1, n + 16);
  assert(p);
  *p = n;
  bytes += n;
  if (bytes > peak)
    peak = bytes;
  return p + 4;
}
void wa_free(void *p) {
  if (p) {
    uint32_t *q = (uint32_t *)p - 4;
    bytes -= *q;
    free(q);
  }
}
static uint64_t cpu(void) {
  struct timespec t;
  assert(!clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t));
  return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void render(int32_t *p, uint32_t n) {
  memset(p, 0, n * 8);
  wa_synth_render(0, p, n, 44100);
}
int main(int argc, char **argv) {
  assert(argc == 2);
  FILE *f = fopen(argv[1], "rb");
  assert(f);
  assert(!fseek(f, 0, SEEK_END));
  long n = ftell(f);
  rewind(f);
  void *data = malloc(n);
  assert(data);
  assert(fread(data, 1, n, f) == (size_t)n);
  fclose(f);
  assert(!wa_synth_load(data, (uint32_t)n));
  assert(synth->fontSamples >= (const short *)data &&
         (const char *)synth->fontSamples < (const char *)data + n);
  assert(tsf_get_presetcount(synth) == 129);
  assert(!wa_bank_full(64, 32) && !wa_bank_full(128, 120) &&
         !wa_bank_full(256, 240) && !wa_bank_full(512, 128) &&
         wa_bank_full(512, 300));
  int32_t a[1024], b[1024];
  wa_synth_reset();
  assert(!wa_synth_short(0x7f3c90));
  render(a, 512);
  assert(wa_synth_voices() == 1);
  int nonzero = 0;
  for (unsigned i = 0; i < 1024; i++)
    nonzero |= a[i] != 0;
  assert(nonzero);
  wa_synth_reset();
  assert(!wa_synth_short(0x01c0));
  assert(program[0] == 1);
  assert(!wa_synth_short(0x7f3c90));
  render(b, 512);
  assert(memcmp(a, b, sizeof(a)));
  wa_synth_reset();
  wa_synth_short(0x7f3c90);
  wa_synth_short(0x0040b0);
  wa_synth_short(0x007bb0);
  render(b, 512); /* all notes off enters release */
  wa_synth_reset();
  wa_synth_short(0x7f40b0);
  wa_synth_short(0x7f3c90);
  wa_synth_short(0x003c80);
  assert(synth->voices[0].heldSustain);
  wa_synth_short(0x0040b0);
  assert(synth->voices[0].ampenv.segment == TSF_SEGMENT_RELEASE);
  wa_synth_reset();
  wa_synth_short(0x7f3c90);
  wa_synth_short(0x0007b0);
  render(b, 512);
  for (unsigned i = 0; i < 1024; i++)
    assert(!b[i]);
  wa_synth_short(0x7f07b0);
  wa_synth_short(0x000bb0);
  render(b, 512);
  for (unsigned i = 0; i < 1024; i++)
    assert(!b[i]);
  wa_synth_short(0x0065b0);
  wa_synth_short(0x0064b0);
  wa_synth_short(0x0c06b0);
  assert(tsf_channel_get_pitchrange(synth, 0) == 12);
  wa_synth_short(0x7f7fe0);
  assert(tsf_channel_get_pitchwheel(synth, 0) == 16383);
  wa_synth_short(0x0079b0);
  assert(bend[0] == 8192 && tsf_channel_get_pitchrange(synth, 0) == 2);
  uint8_t reset[] = {0xf0, 0x7e, 0x7f, 9, 1, 0xf7},
          quiet[] = {0xf0, 0x7f, 0x7f, 4, 1, 0, 0, 0xf7};
  assert(!wa_synth_long(quiet, sizeof(quiet)) && master == 0);
  assert(!wa_synth_long(reset, sizeof(reset)) && master == 16383);
  quiet[3] = 8;
  assert(wa_synth_long(quiet, sizeof(quiet)) == 8);
  assert(wa_synth_long(quiet, 4) == 11);
  wa_synth_reset();
  for (unsigned i = 0; i < 65; i++)
    wa_synth_short(0x7f0090 | ((i + 24) << 8));
  assert(wa_synth_voices() == 64);
  unsigned found = 0;
  for (unsigned i = 0; i < 64; i++)
    found |= synth->voices[i].playingKey == 88;
  assert(found);
  /* Exact batch invariance of the production synth state. */
  wa_synth_reset();
  phase = 0;
  wa_synth_short(0x7f3c90);
  render(a, 512);
  wa_synth_reset();
  phase = 0;
  wa_synth_short(0x7f3c90);
  memset(b, 0, sizeof(b));
  for (unsigned i = 0; i < 512; i++)
    wa_synth_render(0, b + i * 2, 1, 44100);
  assert(!memcmp(a, b, sizeof(a)));
  for (unsigned voices = 1; voices <= 64; voices *= 8) {
    wa_synth_reset();
    for (unsigned i = 0; i < voices; i++)
      wa_synth_short(0x7f0090 | ((i % 48 + 36) << 8));
    uint64_t start = cpu();
    for (unsigned batch = 0; batch < 172; batch++)
      render(b, 256);
    uint64_t elapsed = cpu() - start;
    printf("synth cost authored voices=%u frames=44032 cpu_ns=%llu "
           "ns_per_frame_voice=%.2f\n",
           voices, (unsigned long long)elapsed,
           (double)elapsed / (44032 * voices));
  }
  assert(peak + sizeof(struct wa_engine) < WA_PRIVATE_LIMIT);
  printf("synth private metadata=%u peak=%u engine=%zu decoded_cache=0\n",
         bytes, peak, sizeof(struct wa_engine));
  tsf_close(synth);
  synth = 0;
  assert(!bytes);
  free(data);
  puts("PASS synth "
       "programs/controllers/sustain/RPN/bend/GM-reset/master-volume/"
       "voice-stealing/banks/cache/batches");
  return 0;
}
