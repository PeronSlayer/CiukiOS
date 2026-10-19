/* Real sequencer with authored events and deterministic output-frame clock. */
#include "../../../../sdk/win32/audio/midi.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct wa_engine engine;
static uint32_t messages[20], at[20], count, done_count;
void *wa_alloc(uint32_t n) { return calloc(1, n); }
void wa_free(void *p) { free(p); }
void W32API CiukiAudioLock(void) {}
void W32API CiukiAudioUnlock(void) {}
struct wa_engine *W32API CiukiAudioAcquire(uint32_t rate, uint32_t *err) {
  (void)rate;
  *err = 0;
  return &engine;
}
struct wa_engine *wa_process_engine(void) { return &engine; }
void W32API GlobalMemoryStatus(MEMORYSTATUS *p) { memset(p, 0, sizeof(*p)); }
HANDLE W32API CreateFileA(const char *p, DWORD a, DWORD b,
                          SECURITY_ATTRIBUTES *s, DWORD c, DWORD d, HANDLE h) {
  (void)p;
  (void)a;
  (void)b;
  (void)s;
  (void)c;
  (void)d;
  (void)h;
  return K32_INVALID;
}
DWORD W32API GetFileSize(HANDLE h, DWORD *p) {
  (void)h;
  (void)p;
  return 0;
}
HANDLE W32API CreateFileMappingA(HANDLE h, SECURITY_ATTRIBUTES *s, DWORD a,
                                 DWORD b, DWORD c, const char *p) {
  (void)h;
  (void)s;
  (void)a;
  (void)b;
  (void)c;
  (void)p;
  return 0;
}
BOOL W32API CloseHandle(HANDLE h) {
  (void)h;
  return 1;
}
void *W32API MapViewOfFile(HANDLE h, DWORD a, DWORD b, DWORD c, DWORD d) {
  (void)h;
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  return 0;
}
void w32_diag(const char *a, const char *b, const char *c) {
  (void)a;
  (void)b;
  (void)c;
}
uint32_t wa_synth_load(const void *p, uint32_t n) {
  (void)p;
  (void)n;
  return 0;
}
void wa_synth_render(void *p, int32_t *sum, uint32_t n, uint32_t rate) {
  (void)p;
  (void)sum;
  (void)n;
  (void)rate;
}
uint32_t wa_bank_full(uint32_t a, uint32_t b) {
  (void)a;
  (void)b;
  return 0;
}
uint32_t wa_synth_short(uint32_t msg) {
  assert(count < 20);
  messages[count] = msg;
  at[count++] = (uint32_t)engine.rendered;
  return 0;
}
uint32_t wa_synth_long(const uint8_t *p, uint32_t n) {
  assert(n == 6 && p[0] == 0xf0);
  messages[count] = 0xf0;
  at[count++] = (uint32_t)engine.rendered;
  return 0;
}
void wa_synth_reset(void) {}
void wa_synth_volume(uint32_t v) { (void)v; }
static void dispatch(void *p, const struct wa_event *v) {
  (void)p;
  if (v->message == 0x3c9) {
    struct midi_header *h = (void *)v->p1;
    assert((h->flags & 1) && !(h->flags & 4));
    done_count++;
  }
}
int main(void) {
  struct wa_transport t = {.dispatch = dispatch};
  wa_init(&engine, &t);
  engine.synth = midi_render;
  device = wa_alloc(sizeof(*device));
  device->stream = 1;
  device->paused = 1;
  device->tempo = 500000;
  device->division = 480;
  uintptr_t h = (uintptr_t)device;
  uint32_t events[] = {0,          0,          0x007f3c90, 480,        0,
                       0x0103d090, 480,        0,          0x00003c80, 0,
                       0,          0x80000006, 0x097f7ef0, 0x0000f701};
  struct midi_header header = {.data = (void *)events, .bytes = sizeof(events)};
  assert(!midiOutPrepareHeader(h, &header, sizeof(header)));
  assert(!midiStreamOut(h, &header, sizeof(header)));
  assert(midiStreamOut(h, &header, sizeof(header)) == 65);
  assert(!midiStreamRestart(h));
  int16_t out[512];
  while (engine.rendered < 34080) {
    uint32_t n = 34080 - (uint32_t)engine.rendered;
    if (n > 256)
      n = 256;
    assert(wa_render(&engine, out, n) == n);
    engine.accepted += n;
    wa_consumed(&engine, engine.accepted);
    wa_dispatch(&engine);
  }
  assert(count == 3 && messages[0] == 0x7f3c90 && at[0] == 0 &&
         messages[1] == 0x3c80 && at[1] == 33075 && messages[2] == 0xf0 &&
         at[2] == 33075);
  assert(done_count == 1 && device->pending == 0);
  assert(!midiOutUnprepareHeader(h, &header, sizeof(header)));
  uint32_t property[] = {8, 0xe728};
  assert(!midiStreamProperty(h, property, 0x80000001));
  property[1] = 0;
  assert(!midiStreamProperty(h, property, 0x40000001) && property[1] == 0xe728);
  uint32_t malformed[] = {0, 0, 0x80000040};
  header = (struct midi_header){
      .data = (void *)malformed, .bytes = sizeof(malformed), .flags = 2};
  assert(midiStreamOut(h, &header, sizeof(header)) == 11);
  assert(!midiOutClose(h));
  wa_dispatch(&engine);
  puts("PASS MIDI "
       "tempo/fractional-frames/SMPTE/long-event-padding/header-order/"
       "ownership");
  return 0;
}
