/* GM device and MIDIEVENT sequencer. SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "audio.h"
#include <w32/kernel32.h>
extern void W32API GlobalMemoryStatus(MEMORYSTATUS *);
extern HANDLE W32API CreateFileMappingA(HANDLE, SECURITY_ATTRIBUTES *, DWORD,
                                        DWORD, DWORD, const char *);
extern void *W32API MapViewOfFile(HANDLE, DWORD, DWORD, DWORD, DWORD);
extern uint32_t wa_synth_load(const void *, uint32_t);
extern void wa_synth_render(void *, int32_t *, uint32_t, uint32_t);
extern uint32_t wa_synth_short(uint32_t);
extern uint32_t wa_synth_long(const uint8_t *, uint32_t);
extern void wa_synth_reset(void);
extern void wa_synth_volume(uint32_t);
extern uint32_t wa_bank_full(uint32_t, uint32_t);
extern struct wa_engine *wa_process_engine(void);
struct midi_header {
  char *data;
  uint32_t bytes, recorded;
  uintptr_t user;
  uint32_t flags;
  struct midi_header *next;
  uintptr_t reserved;
  uint32_t offset;
  uintptr_t extra[8];
};
struct midi {
  struct wa_callback callback;
  uint32_t stream, paused, tempo, division, offset, queued, scheduled, pending;
  uint64_t deadline, remainder, next_deadline, next_remainder;
  struct midi_header *headers[32];
};
static struct midi *device;
static uint32_t midi_volume = 0xffffffffu;
static const void *bank;
static HANDLE section;
static uint32_t load_bank(void) {
  if (bank)
    return 0;
  MEMORYSTATUS memory = {.length = sizeof(memory)};
  GlobalMemoryStatus(&memory);
  int full = wa_bank_full(memory.total_phys >> 20, memory.avail_phys >> 20);
  const char *path = full ? "C:\\WINDOWS\\SYSTEM\\FluidR3_GM.sf2"
                          : "C:\\WINDOWS\\SYSTEM\\FluidR3_Ciuki.sf2";
  HANDLE file = CreateFileA(path, 0x80000000, 1, 0, 3, 0, 0);
  if (file == K32_INVALID)
    return 6;
  uint32_t hi = 0, size = GetFileSize(file, &hi);
  if (hi || size > 192 * 1024 * 1024) {
    CloseHandle(file);
    return 7;
  }
  section = CreateFileMappingA(file, 0, 2, 0, 0,
                               full ? "Ciuki.FluidR3.Full.v1"
                                    : "Ciuki.FluidR3.Reduced.v1");
  CloseHandle(file);
  if (!section)
    return 7;
  bank = MapViewOfFile(section, 4, 0, 0, 0);
  if (!bank) {
    CloseHandle(section);
    section = 0;
    return 7;
  }
  uint32_t r = wa_synth_load(bank, size);
  if (r)
    return r;
  w32_diag("audio-bank", "winmm.dll",
           full ? "v=1 bank=FluidR3_GM threshold_mib=384 cache=0 voices=64"
                : "v=1 bank=FluidR3_Ciuki threshold_mib=384 cache=0 voices=64");
  return 0;
}
static struct midi *get(uintptr_t h) {
  return h == (uintptr_t)device ? device : 0;
}
static uint32_t callback(struct midi *d, uint32_t message, uintptr_t p1) {
  struct wa_engine *e = wa_process_engine();
  struct wa_event v = {.due = e->consumed,
                       .callback = d->callback,
                       .handle = (uintptr_t)d,
                       .message = message,
                       .p1 = p1};
  return wa_event(e, &v);
}
static uint32_t midi_done(struct midi *d, struct midi_header *p, uint64_t due) {
  struct wa_event v = {.due = due,
                       .callback = d->callback,
                       .handle = (uintptr_t)d,
                       .message = 0x3c9,
                       .p1 = (uintptr_t)p,
                       .completion_flags = &p->flags,
                       .completion_clear = 4,
                       .completion_set = 1,
                       .completion_count = &d->pending};
  uint32_t r = wa_event(wa_process_engine(), &v);
  if (!r)
    d->pending++;
  return r;
}
void wa_midi_tick(void);
static void midi_render(void *context, int32_t *sum, uint32_t n,
                        uint32_t rate) {
  wa_midi_tick();
  wa_synth_render(context, sum, n, rate);
}
uint32_t W32API midiOutGetNumDevs(void) { return 1; }
static uint32_t caps(uintptr_t id, void *out, uint32_t n, int wide) {
  if (id && id != UINT32_MAX && !get(id))
    return 2;
  if (!out)
    return 11;
  uint8_t p[84] = {0};
  w32_put16(p, 1);
  w32_put16(p + 2, 1);
  w32_put32(p + 4, 0x100);
  const char *s = "CiukiOS General MIDI";
  for (unsigned i = 0; s[i]; i++)
    if (wide)
      w32_put16(p + 8 + i * 2, s[i]);
    else
      p[8 + i] = (uint8_t)s[i];
  unsigned at = wide ? 72 : 40;
  w32_put16(p + at, 2);
  w32_put16(p + at + 2, 64);
  w32_put16(p + at + 4, 64);
  w32_put16(p + at + 6, 65535);
  w32_put32(p + at + 8, 3);
  uint32_t size = wide ? 84 : 52;
  w32_copy(out, p, n < size ? n : size);
  return 0;
}
uint32_t W32API midiOutGetDevCapsA(uintptr_t id, void *p, uint32_t n) {
  return caps(id, p, n, 0);
}
uint32_t W32API midiOutGetDevCapsW(uintptr_t id, void *p, uint32_t n) {
  return caps(id, p, n, 1);
}
uint32_t W32API midiOutOpen(uintptr_t *out, uint32_t id, uintptr_t cb,
                            uintptr_t user, uint32_t flags) {
  if (!out)
    return 11;
  *out = 0;
  if (id && id != UINT32_MAX)
    return 2;
  uint32_t error;
  struct wa_engine *e = CiukiAudioAcquire(44100, &error);
  if (!e)
    return error;
  CiukiAudioLock();
  if (device) {
    CiukiAudioUnlock();
    return 4;
  }
  error = load_bank();
  if (!error) {
    device = wa_alloc(sizeof(*device));
    if (!device)
      error = 7;
    else {
      device->callback = (struct wa_callback){cb, user, flags};
      device->tempo = 500000;
      device->division = 480;
      e->synth = midi_render;
      device->deadline = e->rendered;
      *out = (uintptr_t)device;
      callback(device, 0x3c7, 0);
    }
  }
  CiukiAudioUnlock();
  return error;
}
uint32_t W32API midiOutClose(uintptr_t h) {
  CiukiAudioLock();
  struct midi *d = get(h);
  uint32_t r = !d ? 5 : (d->queued || d->pending) ? 65 : 0;
  if (!r) {
    r = callback(d, 0x3c8, 0);
    if (!r) {
      wa_synth_reset();
      wa_free(d);
      device = 0;
    }
  }
  CiukiAudioUnlock();
  return r;
}
uint32_t W32API midiOutPrepareHeader(uintptr_t h, struct midi_header *p,
                                     uint32_t n) {
  if (!get(h))
    return 5;
  if (!p || n < sizeof(*p) || !p->data)
    return 11;
  if (p->flags & 4)
    return 65;
  p->flags |= 2;
  return 0;
}
uint32_t W32API midiOutUnprepareHeader(uintptr_t h, struct midi_header *p,
                                       uint32_t n) {
  if (!get(h))
    return 5;
  if (!p || n < sizeof(*p))
    return 11;
  if (p->flags & 4)
    return 65;
  p->flags &= ~2u;
  return 0;
}
uint32_t W32API midiOutShortMsg(uintptr_t h, uint32_t msg) {
  CiukiAudioLock();
  uint32_t r = get(h) ? wa_synth_short(msg) : 5;
  CiukiAudioUnlock();
  return r;
}
uint32_t W32API midiOutLongMsg(uintptr_t h, struct midi_header *p, uint32_t n) {
  if (!get(h))
    return 5;
  if (!p || n < sizeof(*p))
    return 11;
  if (!(p->flags & 2))
    return 64;
  if (p->flags & 4)
    return 65;
  CiukiAudioLock();
  struct wa_engine *e = wa_process_engine();
  uint32_t r = e->callback_count == WA_CALLBACKS
                   ? 4
                   : wa_synth_long((const void *)p->data, p->bytes);
  if (!r) {
    p->flags = (p->flags & ~1u) | 4;
    midi_done(device, p, e->consumed);
  }
  CiukiAudioUnlock();
  return r;
}
uint32_t W32API midiOutReset(uintptr_t h) {
  CiukiAudioLock();
  struct midi *d = get(h);
  uint32_t r = !d ? 5 : 0;
  if (d) {
    if (d->queued > WA_CALLBACKS - wa_process_engine()->callback_count)
      r = 4;
    else {
      for (unsigned i = 0; i < d->queued; i++) {
        midi_done(d, d->headers[i], wa_process_engine()->consumed);
      }
      for (unsigned i = 0; i < wa_process_engine()->callback_count; i++) {
        struct wa_event *v = &wa_process_engine()->events[i];
        if (v->handle == h && v->message == 0x3c9) {
          v->due = wa_process_engine()->consumed;
          if (v->completion_flags) {
            *v->completion_flags = (*v->completion_flags & ~4u) | 1;
            v->completion_flags = 0;
          }
        }
      }
      d->queued = d->offset = d->scheduled = 0;
      d->deadline = wa_process_engine()->rendered;
      d->remainder = 0;
      wa_synth_reset();
    }
  }
  CiukiAudioUnlock();
  return r;
}
uint32_t W32API midiOutSetVolume(uintptr_t h, uint32_t v) {
  if (h && h != UINT32_MAX && !get(h))
    return 5;
  CiukiAudioLock();
  midi_volume = v;
  wa_synth_volume(v);
  CiukiAudioUnlock();
  return 0;
}
uint32_t W32API midiOutGetVolume(uintptr_t h, uint32_t *v) {
  if (!v)
    return 11;
  if (h && h != UINT32_MAX && !get(h))
    return 5;
  *v = midi_volume;
  return 0;
}
uint32_t W32API midiOutGetID(uintptr_t h, uint32_t *id) {
  if (!get(h))
    return 5;
  if (!id)
    return 11;
  *id = 0;
  return 0;
}
uint32_t W32API midiStreamOpen(uintptr_t *out, uint32_t *id, uint32_t count,
                               uintptr_t cb, uintptr_t user, uint32_t flags) {
  if (!id || count != 1)
    return 11;
  uint32_t r = midiOutOpen(out, *id, cb, user, flags);
  if (!r) {
    device->stream = 1;
    device->paused = 1;
  }
  return r;
}
uint32_t W32API midiStreamClose(uintptr_t h) { return midiOutClose(h); }
uint32_t W32API midiStreamProperty(uintptr_t h, uint32_t *p, uint32_t flags) {
  struct midi *d = get(h);
  if (!d)
    return 5;
  if (!p || p[0] != 8 || !(flags & 0xc0000000) ||
      (flags & 0xc0000000) == 0xc0000000 || !(flags & 3) || (flags & 3) == 3)
    return 11;
  uint32_t *v = flags & 2 ? &d->tempo : &d->division;
  if (flags & 0x80000000) {
    if (!p[1] || ((flags & 2) && p[1] > 0xffffff))
      return 11;
    if (flags & 1) {
      if (p[1] > 65535)
        return 11;
      if (p[1] & 0x8000) {
        int fps = -(int8_t)(p[1] >> 8);
        if (!(p[1] & 255) || (fps != 24 && fps != 25 && fps != 29 && fps != 30))
          return 11;
      }
    }
    *v = p[1];
  } else
    p[1] = *v;
  return 0;
}
static int valid_events(const struct midi_header *p) {
  uint32_t at = 0;
  while (at < p->bytes) {
    if (p->bytes - at < 12)
      return 0;
    const uint8_t *b = (const void *)p->data;
    uint32_t event = w32_u32(b + at + 8), step = 12;
    if (((event >> 24 & 0xbf) == 1 && !(event & 0xffffff)) ||
        w32_u32(b + at + 4))
      return 0;
    if (event & 0x80000000) {
      uint32_t n = event & 0xffffff;
      step += (n + 3) & ~3u;
    }
    if (step > p->bytes - at)
      return 0;
    at += step;
  }
  return 1;
}
uint32_t W32API midiStreamOut(uintptr_t h, struct midi_header *p, uint32_t n) {
  struct midi *d = get(h);
  if (!d || !d->stream)
    return 5;
  if (!p || n < sizeof(*p) || !p->data || p->bytes >= 65536 || !valid_events(p))
    return 11;
  if (!(p->flags & 2))
    return 64;
  if (p->flags & 4)
    return 65;
  CiukiAudioLock();
  if (d->queued == 32) {
    CiukiAudioUnlock();
    return 4;
  }
  p->flags = (p->flags & ~1u) | 4;
  d->headers[d->queued++] = p;
  CiukiAudioUnlock();
  return 0;
}
uint32_t W32API midiStreamRestart(uintptr_t h) {
  struct midi *d = get(h);
  if (!d || !d->stream)
    return 5;
  d->paused = 0;
  return 0;
}
uint32_t W32API midiStreamPause(uintptr_t h) {
  struct midi *d = get(h);
  if (!d || !d->stream)
    return 5;
  d->paused = 1;
  return 0;
}
uint32_t W32API midiStreamStop(uintptr_t h) {
  uint32_t r = midiOutReset(h);
  if (!r)
    device->paused = 1;
  return r;
}
void wa_midi_tick(void) {
  struct midi *d = device;
  if (!d || !d->stream)
    return;
  struct wa_engine *e = wa_process_engine();
  uint64_t now = e->rendered;
  if (d->paused) {
    d->deadline++;
    if (d->scheduled)
      d->next_deadline++;
    return;
  }
  if (!d->queued) {
    d->deadline = now;
    return;
  }
  for (unsigned quota = 0; quota < 64 && d->queued; quota++) {
    struct midi_header *p = d->headers[0];
    if (d->offset == p->bytes) {
      if (midi_done(d, p, now + 1))
        return;
      for (unsigned i = 0; i + 1 < d->queued; i++)
        d->headers[i] = d->headers[i + 1];
      d->queued--;
      d->offset = d->scheduled = 0;
      continue;
    }
    const uint8_t *b = (const void *)p->data;
    uint32_t event = w32_u32(b + d->offset + 8);
    if (!d->scheduled) {
      uint32_t delta = w32_u32(b + d->offset);
      uint64_t duration = (uint64_t)delta * d->tempo,
               denom = (uint64_t)d->division * 1000000;
      if (d->division & 0x8000) {
        unsigned fps = (unsigned)-(int8_t)(d->division >> 8);
        duration = (uint64_t)delta * (fps == 29 ? 1001 : 1);
        denom = (fps == 29 ? 30000 : fps) * (d->division & 255);
      }
      uint64_t rem = (duration % denom) * e->rate + d->remainder;
      d->next_deadline =
          d->deadline + (duration / denom) * e->rate + rem / denom;
      d->next_remainder = rem % denom;
      d->scheduled = 1;
    }
    if (now < d->next_deadline)
      return;
    if (e->callback_count + 2 >= WA_CALLBACKS)
      return;
    d->deadline = d->next_deadline;
    d->remainder = d->next_remainder;
    d->scheduled = 0;
    if (event & 0x40000000) {
      struct wa_event v = {.due = now + 1,
                           .callback = d->callback,
                           .handle = (uintptr_t)d,
                           .message = 0x3ca,
                           .p1 = (uintptr_t)p};
      wa_event(e, &v);
    }
    d->offset += 12;
    uint32_t type = event >> 24 & 0xbf;
    if (type == 1)
      d->tempo = event & 0xffffff;
    else if (type == 0)
      wa_synth_short(event & 0xffffff);
    else if (type == 0x80)
      wa_synth_long(b + d->offset, event & 0xffffff);
    if (event & 0x80000000)
      d->offset += ((event & 0xffffff) + 3) & ~3u;
  }
}
