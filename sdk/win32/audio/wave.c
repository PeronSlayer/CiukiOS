/* WinMM PCM facade. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "audio.h"
#define API W32API
extern struct wa_engine *wa_process_engine(void);
struct wave {
  struct wa_source source;
  uint32_t magic;
};
static struct wave *waves[16];
static struct wave *get(uintptr_t h) {
  for (unsigned i = 0; i < 16; i++)
    if ((uintptr_t)waves[i] == h)
      return waves[i];
  return 0;
}
static uint32_t wide_caps(uintptr_t id, void *out, uint32_t bytes, int wide) {
  if (id != 0 && id != UINT32_MAX && !get(id))
    return 2;
  if (!out)
    return 11;
  uint8_t data[84] = {0};
  w32_put16(data, 1);
  w32_put16(data + 2, 1);
  w32_put32(data + 4, 0x100);
  const char *name = "CiukiOS audio";
  for (unsigned i = 0; name[i]; i++)
    if (wide)
      w32_put16(data + 8 + i * 2, name[i]);
    else
      data[8 + i] = (uint8_t)name[i];
  unsigned at = wide ? 72 : 40;
  w32_put32(data + at, 0xfff);
  w32_put16(data + at + 4, 2);
  w32_put32(data + at + 8, 0xc);
  w32_copy(out, data, bytes < (wide ? 84u : 52u) ? bytes : (wide ? 84u : 52u));
  return 0;
}
uint32_t API waveOutGetNumDevs(void) { return 1; }
uint32_t API waveOutGetDevCapsA(uintptr_t id, void *out, uint32_t bytes) {
  return wide_caps(id, out, bytes, 0);
}
uint32_t API waveOutGetDevCapsW(uintptr_t id, void *out, uint32_t bytes) {
  return wide_caps(id, out, bytes, 1);
}
uint32_t API waveOutOpen(uintptr_t *out, uint32_t id, const struct wa_format *f,
                         uintptr_t callback, uintptr_t instance,
                         uint32_t flags) {
  if (id != 0 && id != UINT32_MAX)
    return 2;
  if (!wa_valid_format(f))
    return 32;
  if (flags & 1)
    return 0;
  if (!out)
    return 11;
  *out = 0;
  if ((flags & 0x70000) != 0 && (flags & 0x70000) != WA_CALLBACK_FUNCTION &&
      (flags & 0x70000) != WA_CALLBACK_EVENT &&
      (flags & 0x70000) != WA_CALLBACK_WINDOW &&
      (flags & 0x70000) != WA_CALLBACK_THREAD)
    return 10;
  uint32_t error;
  struct wa_engine *e = CiukiAudioAcquire(f->rate, &error);
  if (!e)
    return error;
  CiukiAudioLock();
  unsigned i;
  for (i = 0; i < 16 && waves[i]; i++) {
  }
  if (i == 16) {
    CiukiAudioUnlock();
    return WA_ALLOCATED;
  }
  struct wave *w = wa_alloc(sizeof(*w));
  if (!w) {
    CiukiAudioUnlock();
    return WA_NOMEM;
  }
  wa_source_init(&w->source, f);
  w->source.wave = 1;
  w->source.callback = (struct wa_callback){callback, instance, flags};
  w->source.handle = (uintptr_t)w;
  error = wa_register(e, &w->source);
  if (error) {
    wa_free(w);
    CiukiAudioUnlock();
    return error;
  }
  waves[i] = w;
  *out = (uintptr_t)w;
  struct wa_event v = {.due = e->consumed,
                       .callback = w->source.callback,
                       .handle = *out,
                       .message = 0x3bb};
  error = wa_event(e, &v);
  CiukiAudioUnlock();
  return error;
}
uint32_t API waveOutClose(uintptr_t h) {
  CiukiAudioLock();
  struct wave *w = get(h);
  if (!w) {
    CiukiAudioUnlock();
    return 5;
  }
  for (unsigned i = 0; i < w->source.header_count; i++)
    if (w->source.headers[i]->flags & WA_QUEUED) {
      CiukiAudioUnlock();
      return 33;
    }
  struct wa_engine *e = wa_process_engine();
  struct wa_event v = {.due = e->consumed,
                       .callback = w->source.callback,
                       .handle = h,
                       .message = 0x3bc};
  uint32_t r = wa_event(e, &v);
  if (!r) {
    wa_unregister(e, &w->source);
    for (unsigned i = 0; i < 16; i++)
      if (waves[i] == w)
        waves[i] = 0;
    wa_free(w);
  }
  CiukiAudioUnlock();
  return r;
}
uint32_t API waveOutPrepareHeader(uintptr_t h, struct wa_header *p,
                                  uint32_t n) {
  if (!get(h))
    return 5;
  if (!p || n < sizeof(*p) || !p->data)
    return 11;
  if (p->flags & WA_QUEUED)
    return 33;
  p->flags = (p->flags & ~WA_DONE) | WA_PREPARED;
  return 0;
}
uint32_t API waveOutUnprepareHeader(uintptr_t h, struct wa_header *p,
                                    uint32_t n) {
  if (!get(h))
    return 5;
  if (!p || n < sizeof(*p))
    return 11;
  if (p->flags & WA_QUEUED)
    return 33;
  p->flags &= ~WA_PREPARED;
  return 0;
}
uint32_t API waveOutWrite(uintptr_t h, struct wa_header *p, uint32_t n) {
  CiukiAudioLock();
  struct wave *w = get(h);
  uint32_t r = !w               ? 5
               : n < sizeof(*p) ? 11
                                : wa_write(wa_process_engine(), &w->source, p);
  CiukiAudioUnlock();
  return r;
}
uint32_t API waveOutReset(uintptr_t h) {
  CiukiAudioLock();
  struct wave *w = get(h);
  uint32_t r = w ? wa_reset(wa_process_engine(), &w->source) : 5;
  CiukiAudioUnlock();
  return r;
}
static uint32_t pause(uintptr_t h, uint32_t paused) {
  CiukiAudioLock();
  struct wave *w = get(h);
  if (w)
    w->source.paused = paused;
  CiukiAudioUnlock();
  return w ? 0 : 5;
}
uint32_t API waveOutPause(uintptr_t h) { return pause(h, 1); }
uint32_t API waveOutRestart(uintptr_t h) { return pause(h, 0); }
uint32_t API waveOutBreakLoop(uintptr_t h) {
  struct wave *w = get(h);
  if (!w)
    return 5;
  w->source.break_loop = 1;
  return 0;
}
struct mmtime {
  uint32_t type;
  uint32_t value[2];
};
uint32_t API waveOutGetPosition(uintptr_t h, struct mmtime *out, uint32_t n) {
  if (!out || n < sizeof(*out))
    return 11;
  CiukiAudioLock();
  struct wave *w = get(h);
  uint32_t r = 5;
  if (w) {
    struct wa_engine *e = wa_process_engine();
    if (wa_poll(e)) {
      CiukiAudioUnlock();
      return 6;
    }
    uint64_t samples = w->source.heard_samples;
    if (out->type == 4)
      out->value[0] = (uint32_t)samples * w->source.format.align;
    else if (out->type == 2)
      out->value[0] = (uint32_t)samples;
    else {
      out->type = 1;
      out->value[0] = (uint32_t)(samples * 1000 / w->source.format.rate);
    }
    r = 0;
  }
  CiukiAudioUnlock();
  return r;
}
uint32_t API waveOutGetVolume(uintptr_t h, uint32_t *out) {
  if (!out)
    return 11;
  if (h && h != UINT32_MAX && !get(h))
    return 5;
  CiukiAudioLock();
  struct wa_engine *e = wa_process_engine();
  *out = e->volume[0] | e->volume[1] << 16;
  CiukiAudioUnlock();
  return 0;
}
uint32_t API waveOutSetVolume(uintptr_t h, uint32_t v) {
  if (h && h != UINT32_MAX && !get(h))
    return 5;
  CiukiAudioLock();
  struct wa_engine *e = wa_process_engine();
  e->volume[0] = v & 65535;
  e->volume[1] = v >> 16;
  CiukiAudioUnlock();
  return 0;
}
uint32_t API waveOutGetID(uintptr_t h, uint32_t *id) {
  if (!get(h))
    return 5;
  if (!id)
    return 11;
  *id = 0;
  return 0;
}
uint32_t API waveOutGetPitch(uintptr_t h, uint32_t *p) {
  (void)p;
  return get(h) ? 8 : 5;
}
uint32_t API waveOutGetPlaybackRate(uintptr_t h, uint32_t *p) {
  return waveOutGetPitch(h, p);
}
uint32_t API waveOutSetPitch(uintptr_t h, uint32_t p) {
  return waveOutGetPitch(h, &p);
}
uint32_t API waveOutSetPlaybackRate(uintptr_t h, uint32_t p) {
  return waveOutGetPitch(h, &p);
}
uint32_t API waveOutMessage(uintptr_t h, uint32_t m, uintptr_t a, uintptr_t b) {
  (void)m;
  (void)a;
  (void)b;
  return get(h) ? 8 : 5;
}
uint32_t API waveInGetNumDevs(void) { return 0; }
uint32_t API midiInGetNumDevs(void) { return 0; }
/* Capture is deliberately absent. Never manufacture input handles. */
uint32_t W32API waveInOpen(uintptr_t *h, uint32_t id, const void *f,
                           uintptr_t cb, uintptr_t user, uint32_t flags) {
  (void)id;
  (void)f;
  (void)cb;
  (void)user;
  (void)flags;
  if (h)
    *h = 0;
  return 6;
}
uint32_t W32API midiInOpen(uintptr_t *h, uint32_t id, uintptr_t cb,
                           uintptr_t user, uint32_t flags) {
  return waveInOpen(h, id, 0, cb, user, flags);
}
uint32_t W32API waveInGetDevCapsA(uintptr_t id, void *p, uint32_t n) {
  (void)id;
  (void)p;
  (void)n;
  return 2;
}
uint32_t W32API waveInGetDevCapsW(uintptr_t id, void *p, uint32_t n) {
  return waveInGetDevCapsA(id, p, n);
}
uint32_t W32API midiInGetDevCapsA(uintptr_t id, void *p, uint32_t n) {
  return waveInGetDevCapsA(id, p, n);
}
uint32_t W32API midiInGetDevCapsW(uintptr_t id, void *p, uint32_t n) {
  return waveInGetDevCapsA(id, p, n);
}
#define NO_INPUT_ONE(name)                                                     \
  uint32_t W32API name(uintptr_t h) {                                          \
    (void)h;                                                                   \
    return 5;                                                                  \
  }
#define NO_INPUT_HEADER(name)                                                  \
  uint32_t W32API name(uintptr_t h, void *p, uint32_t n) {                     \
    (void)h;                                                                   \
    (void)p;                                                                   \
    (void)n;                                                                   \
    return 5;                                                                  \
  }
NO_INPUT_ONE(waveInClose)
NO_INPUT_ONE(waveInReset)
NO_INPUT_ONE(waveInStart)
NO_INPUT_ONE(waveInStop)
NO_INPUT_ONE(midiInClose)
NO_INPUT_ONE(midiInReset) NO_INPUT_ONE(midiInStart) NO_INPUT_ONE(midiInStop)
    NO_INPUT_HEADER(waveInPrepareHeader) NO_INPUT_HEADER(waveInUnprepareHeader)
        NO_INPUT_HEADER(waveInAddBuffer) NO_INPUT_HEADER(waveInGetPosition)
            NO_INPUT_HEADER(midiInPrepareHeader)
                NO_INPUT_HEADER(midiInUnprepareHeader)
                    NO_INPUT_HEADER(midiInAddBuffer)
