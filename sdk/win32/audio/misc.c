/* Small WinMM device and mixer answers. LGPL-2.1-or-later. */
#include "audio.h"
#include <w32/services.h>
extern struct wa_engine *wa_process_engine(void);
extern void wa_master(uint32_t[2]);
uint32_t W32API auxGetNumDevs(void) { return 0; }
static uint32_t mixer_refs;
uint32_t W32API mixerGetNumDevs(void) { return 1; }
uint32_t W32API mixerOpen(uintptr_t *h, uint32_t id, uintptr_t cb,
                          uintptr_t instance, uint32_t flags) {
  (void)cb;
  (void)instance;
  (void)flags;
  if (!h)
    return 11;
  if (id)
    return 2;
  uint32_t r;
  if (!CiukiAudioAcquire(44100, &r))
    return r;
  CiukiAudioLock();
  mixer_refs++;
  *h = 0x434d4958;
  CiukiAudioUnlock();
  return 0;
}
uint32_t W32API mixerClose(uintptr_t h) {
  if (h != 0x434d4958 || !mixer_refs)
    return 5;
  mixer_refs--;
  return 0;
}
static int mixer_valid(uintptr_t h) {
  return !h || (h == 0x434d4958 && mixer_refs);
}
static void text(void *out, const char *s, unsigned limit, int wide) {
  uint8_t *p = out;
  for (unsigned i = 0; i < limit; i++) {
    unsigned c = *s ? (uint8_t)*s++ : 0;
    if (wide)
      w32_put16(p + i * 2, c);
    else
      p[i] = (uint8_t)c;
  }
}
struct line {
  uint32_t size, destination, source, id, flags;
  uintptr_t user;
  uint32_t component, channels, connections, controls;
};
static uint32_t lineinfo(uintptr_t h, struct line *p, uint32_t flags,
                         int wide) {
  if (!mixer_valid(h))
    return 5;
  if (!p || p->size < (wide ? 280u : 168u))
    return 11;
  uint32_t query = flags & 15, id = 0;
  if (query == 1) {
    if (p->destination || p->source)
      return 1024;
    id = 1;
  } else if (query == 2) {
    if (p->id > 1)
      return 1024;
    id = p->id;
  } else if (query == 3) {
    if (p->component != 4 && p->component != 0x1008)
      return 1024;
    id = p->component == 0x1008;
  } else if (query || p->destination)
    return 1024;
  uint32_t size = p->size;
  w32_fill(p, 0, wide ? 280 : 168);
  p->size = size;
  p->id = id;
  p->flags = 1 | (id ? 0x80000000u : 0);
  p->component = id ? 0x1008 : 4;
  p->channels = 2;
  p->connections = id ? 0 : 1;
  p->controls = 1;
  uint8_t *b = (void *)p;
  text(b + sizeof(*p), id ? "Wave" : "Master", 16, wide);
  text(b + sizeof(*p) + (wide ? 32 : 16),
       id ? "CiukiOS wave output" : "CiukiOS master volume", 64, wide);
  return 0;
}
uint32_t W32API mixerGetLineInfoA(uintptr_t h, struct line *p, uint32_t f) {
  return lineinfo(h, p, f, 0);
}
uint32_t W32API mixerGetLineInfoW(uintptr_t h, struct line *p, uint32_t f) {
  return lineinfo(h, p, f, 1);
}
struct controls {
  uint32_t size, line, control, count, control_size;
  void *controls;
};
static uint32_t linecontrols(uintptr_t h, struct controls *p, uint32_t flags,
                             int wide) {
  if (!mixer_valid(h))
    return 5;
  if (!p || p->size != sizeof(*p) || !p->controls || p->count != 1 ||
      p->control_size < (wide ? 228u : 148u))
    return 11;
  uint32_t mode = flags & 15, id = 0;
  if (mode == 1) {
    if (p->control < 1 || p->control > 2)
      return 1025;
    id = p->control;
  } else {
    if (p->line > 1)
      return 1024;
    if (mode == 2 && p->control != 0x50030001)
      return 1025;
    if (mode > 2)
      return 10;
    id = p->line + 1;
  }
  uint8_t *c = p->controls;
  w32_fill(c, 0, wide ? 228 : 148);
  w32_put32(c, wide ? 228 : 148);
  w32_put32(c + 4, id);
  w32_put32(c + 8, 0x50030001);
  text(c + 20, id == 1 ? "Master" : "Wave", 16, wide);
  text(c + 20 + (wide ? 32 : 16), id == 1 ? "Master volume" : "Wave volume", 64,
       wide);
  unsigned at = wide ? 180 : 100;
  w32_put32(c + at + 4, 65535);
  w32_put32(c + at + 24, 65536);
  return 0;
}
uint32_t W32API mixerGetLineControlsA(uintptr_t h, struct controls *p,
                                      uint32_t f) {
  return linecontrols(h, p, f, 0);
}
uint32_t W32API mixerGetLineControlsW(uintptr_t h, struct controls *p,
                                      uint32_t f) {
  return linecontrols(h, p, f, 1);
}
struct details {
  uint32_t size, control, channels;
  uintptr_t owner;
  uint32_t detail_size;
  void *details;
};
uint32_t W32API mixerGetControlDetailsA(uintptr_t h, struct details *p,
                                        uint32_t flags) {
  if (!mixer_valid(h))
    return 5;
  if (!p || p->size != sizeof(*p) || !p->details || p->detail_size != 4 ||
      (p->channels != 1 && p->channels != 2) || (flags & 15))
    return 11;
  if (p->control != 1 && p->control != 2)
    return 1025;
  CiukiAudioLock();
  uint32_t v[2];
  if (p->control == 1)
    wa_master(v);
  else {
    struct wa_engine *e = wa_process_engine();
    v[0] = e->volume[0];
    v[1] = e->volume[1];
  }
  for (unsigned i = 0; i < p->channels; i++)
    ((uint32_t *)p->details)[i] = v[i];
  CiukiAudioUnlock();
  return 0;
}
uint32_t W32API mixerGetControlDetailsW(uintptr_t h, struct details *p,
                                        uint32_t f) {
  return mixerGetControlDetailsA(h, p, f);
}
uint32_t W32API mixerSetControlDetails(uintptr_t h, struct details *p,
                                       uint32_t flags) {
  if (!mixer_valid(h))
    return 5;
  if (!p || p->size != sizeof(*p) || !p->details || p->detail_size != 4 ||
      (p->channels != 1 && p->channels != 2) || (flags & 15))
    return 11;
  if (p->control != 1 && p->control != 2)
    return 1025;
  uint32_t *v = p->details;
  if (v[0] > 65535 || (p->channels == 2 && v[1] > 65535))
    return 11;
  if (p->control == 1) {
    w32_diag("audio", "winmm.dll", "master-write-ignored");
    return 0;
  }
  CiukiAudioLock();
  struct wa_engine *e = wa_process_engine();
  e->volume[0] = v[0];
  e->volume[1] = v[p->channels == 2 ? 1 : 0];
  CiukiAudioUnlock();
  return 0;
}
uint32_t W32API mixerMessage(uintptr_t h, uint32_t msg, uintptr_t a,
                             uintptr_t b) {
  (void)msg;
  (void)a;
  (void)b;
  return mixer_valid(h) ? 8 : 5;
}
uint32_t W32API mixerGetID(uintptr_t h, uint32_t *id, uint32_t flags) {
  (void)flags;
  if (!mixer_valid(h))
    return 5;
  if (!id)
    return 11;
  *id = 0;
  return 0;
}
/* MCI lives in mci.c, mcistr.c and the device drivers (f5-12B). */
