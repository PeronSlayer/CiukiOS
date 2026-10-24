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
/* MIDI input remains unavailable; wave input uses the copied native capture. */
uint32_t API midiInGetNumDevs(void) { return 0; }
uint32_t API midiInOpen(uintptr_t *h, uint32_t id, uintptr_t cb, uintptr_t user, uint32_t flags)
{ (void)id; (void)cb; (void)user; (void)flags; if (h) *h=0; return 6; }
uint32_t API midiInGetDevCapsA(uintptr_t id, void *p, uint32_t n)
{ (void)id; (void)p; (void)n; return 2; }
uint32_t API midiInGetDevCapsW(uintptr_t id, void *p, uint32_t n)
{ return midiInGetDevCapsA(id,p,n); }
#define NO_INPUT_ONE(name) uint32_t API name(uintptr_t h) { (void)h; return 5; }
#define NO_INPUT_HEADER(name) uint32_t API name(uintptr_t h, void *p, uint32_t n) { (void)h; (void)p; (void)n; return 5; }
NO_INPUT_ONE(midiInClose)
NO_INPUT_ONE(midiInReset)
NO_INPUT_ONE(midiInStart)
NO_INPUT_ONE(midiInStop)
NO_INPUT_HEADER(midiInPrepareHeader)
NO_INPUT_HEADER(midiInUnprepareHeader)
NO_INPUT_HEADER(midiInAddBuffer)

#include <ciuki/abi.h>
struct wave_input {
  struct wa_format format;
  struct wa_callback callback;
  struct wa_header *prepared[WA_HEADERS], *queue[WA_HEADERS];
  uint32_t count, phase, frames, running, failed;
  int fd;
};
static struct wave_input *input;
static uint32_t input_error(int e) {
  return e == -EBUSY ? WA_ALLOCATED : e == -ENOMEM ? WA_NOMEM :
         e == -EINVAL ? WA_INVALID : 6;
}
static int input_ctl(int fd, uint32_t op, void *p, uint32_t n) {
  return wa_capture_native(CIUKI_SYS_DEVCTL, (uintptr_t)fd, op, (uintptr_t)p, n);
}
static int input_fd(void) {
  return wa_capture_native(CIUKI_SYS_OPEN, (uintptr_t)"/dev/audio", O_RDONLY | O_NONBLOCK, 0, 0);
}
static void input_close_fd(int fd) {
  if (fd >= 0) wa_capture_native(CIUKI_SYS_CLOSE, (uintptr_t)fd, 0, 0, 0);
}
uint32_t API waveInGetNumDevs(void) {
  int fd = input_fd();
  if (fd < 0) return 0;
  struct ciuki_audio_capture c = {.version=1,.size=sizeof(c)};
  int r = input_ctl(fd,AUDIO_CAPTURE_CAPS,&c,sizeof(c)); input_close_fd(fd);
  return !r && c.available ? 1 : 0;
}
static int input_valid(const struct wa_format *f) {
  return wa_valid_format(f) && f->tag == 1 && f->rate <= 48000;
}
static uint32_t input_event(uint32_t message, struct wa_header *h) {
  struct wa_engine *e = wa_process_engine();
  struct wa_event event = {.due=e->consumed,.callback=input->callback,
    .handle=(uintptr_t)input,.message=message,.p1=(uintptr_t)h};
  return wa_event(e,&event);
}
static uint32_t input_caps(uintptr_t id, void *p, uint32_t n, int wide) {
  if (id != 0 && id != UINT32_MAX && id != (uintptr_t)input) return 2;
  if (!waveInGetNumDevs()) return 2;
  if (!p) return WA_INVALID;
  uint8_t caps[80] = {0};
  w32_put16(caps,1); w32_put16(caps+2,1); w32_put32(caps+4,0x100);
  const char *name="CiukiOS capture";
  for (unsigned i=0;name[i];i++) {
    if (wide) w32_put16(caps+8+i*2,(uint16_t)name[i]);
    else caps[8+i]=(uint8_t)name[i];
  }
  unsigned at=wide ? 72 : 40;
  w32_put32(caps+at,0xfff); w32_put16(caps+at+4,2);
  unsigned size=wide ? 80 : 48;
  w32_copy(p,caps,n<size ? n : size); return 0;
}
uint32_t API waveInGetDevCapsA(uintptr_t id, void *p, uint32_t n) { return input_caps(id,p,n,0); }
uint32_t API waveInGetDevCapsW(uintptr_t id, void *p, uint32_t n) { return input_caps(id,p,n,1); }
uint32_t API waveInOpen(uintptr_t *out, uint32_t id, const struct wa_format *f,
                       uintptr_t cb, uintptr_t user, uint32_t flags) {
  if (out) *out=0;
  if (id != 0 && id != UINT32_MAX) return 2;
  if (!input_valid(f)) return 32;
  if (!waveInGetNumDevs()) return 6;
  if (flags & 1) return 0;
  if (!out) return WA_INVALID;
  uint32_t kind=flags & 0x70000;
  if ((flags & ~0x7000bu) || (kind && kind!=WA_CALLBACK_WINDOW && kind!=WA_CALLBACK_THREAD &&
      kind!=WA_CALLBACK_FUNCTION && kind!=WA_CALLBACK_EVENT)) return 10;
  uint32_t error;
  if (!CiukiAudioAcquire(0,&error)) return error;
  CiukiAudioLock();
  if (input) { CiukiAudioUnlock(); return WA_ALLOCATED; }
  struct wave_input *w=wa_alloc(sizeof(*w));
  if (!w) { CiukiAudioUnlock(); return WA_NOMEM; }
  *w=(struct wave_input){0}; w->fd=input_fd();
  if (w->fd<0) { error=input_error(w->fd); wa_free(w); CiukiAudioUnlock(); return error; }
  struct ciuki_audio_capture reservation={.version=1,.size=sizeof(reservation)};
  int reserve=input_ctl(w->fd,AUDIO_CAPTURE_ACQUIRE,&reservation,sizeof(reservation));
  if (reserve<0) { input_close_fd(w->fd); wa_free(w); CiukiAudioUnlock(); return input_error(reserve); }
  w->format=*f; w->callback=(struct wa_callback){cb,user,flags}; input=w;
  error=input_event(0x3be,0); /* WIM_OPEN */
  if (error) { input=0; input_close_fd(w->fd); wa_free(w); }
  else *out=(uintptr_t)w;
  CiukiAudioUnlock(); return error;
}
static int input_prepared(struct wa_header *h) {
  for (unsigned i=0;i<WA_HEADERS;i++) if (input->prepared[i]==h) return (int)i;
  return -1;
}
uint32_t API waveInPrepareHeader(uintptr_t h, struct wa_header *p, uint32_t n) {
  CiukiAudioLock(); uint32_t r=0;
  if (!input || h!=(uintptr_t)input) r=5;
  else if (!p || n<sizeof(*p) || !p->data || !p->bytes || p->bytes%input->format.align) r=WA_INVALID;
  else if (p->flags & WA_QUEUED) r=33;
  else if (input_prepared(p)<0) {
    unsigned i; for (i=0;i<WA_HEADERS && input->prepared[i];i++) {}
    if (i==WA_HEADERS) r=WA_NOMEM;
    else { input->prepared[i]=p; p->flags=(p->flags & ~WA_DONE)|WA_PREPARED; p->recorded=0; }
  }
  CiukiAudioUnlock(); return r;
}
uint32_t API waveInUnprepareHeader(uintptr_t h, struct wa_header *p, uint32_t n) {
  CiukiAudioLock(); uint32_t r=0;
  if (!input || h!=(uintptr_t)input) r=5;
  else if (!p || n<sizeof(*p)) r=WA_INVALID;
  else if (p->flags & WA_QUEUED) r=33;
  else { int i=input_prepared(p); if (i>=0) input->prepared[i]=0; p->flags &= ~WA_PREPARED; }
  CiukiAudioUnlock(); return r;
}
uint32_t API waveInAddBuffer(uintptr_t h, struct wa_header *p, uint32_t n) {
  CiukiAudioLock(); uint32_t r=0;
  if (!input || h!=(uintptr_t)input) r=5;
  else if (!p || n<sizeof(*p) || !p->data || !p->bytes || p->bytes%input->format.align) r=WA_INVALID;
  else if (input_prepared(p)<0 || !(p->flags & WA_PREPARED)) r=34;
  else if ((p->flags & WA_QUEUED) || input->count==WA_HEADERS) r=33;
  else { p->recorded=0; p->flags=(p->flags & ~WA_DONE)|WA_QUEUED; input->queue[input->count++]=p; }
  CiukiAudioUnlock(); return r;
}
static int input_done(void) {
  struct wa_header *h=input->queue[0];
  if (input_event(0x3c0,h)) return 0; /* WIM_DATA; retain buffer on callback quota */
  h->flags=(h->flags & ~WA_QUEUED)|WA_DONE;
  for (unsigned i=1;i<input->count;i++) input->queue[i-1]=input->queue[i];
  input->count--; return 1;
}
static int input_stop(void) {
  if (!input->running) return 0;
  struct ciuki_audio_capture c={.version=1,.size=sizeof(c)};
  int r=input_ctl(input->fd,AUDIO_CAPTURE_STOP,&c,sizeof(c)); input->running=0;
  if (r<0) input->failed=1;
  return r;
}
uint32_t API waveInStart(uintptr_t h) {
  CiukiAudioLock(); uint32_t r=0;
  if (!input || h!=(uintptr_t)input) r=5;
  else if (input->failed) r=6;
  else if (!input->running) {
    struct ciuki_audio_capture c={.version=1,.size=sizeof(c),.gain=100,.rate=48000,
      .channels=2,.format=CIUKI_AUDIO_S16LE,.source=CIUKI_CAPTURE_MIC};
    struct ciuki_audio_capture preferred={.version=1,.size=sizeof(preferred)};
    int e=input_ctl(input->fd,AUDIO_CAPTURE_CAPS,&preferred,sizeof(preferred));
    if (!e && preferred.source<=CIUKI_CAPTURE_MIX) c.source=preferred.source;
    if (!e) e=input_ctl(input->fd,AUDIO_CAPTURE_START,&c,sizeof(c));
    if (e<0) r=input_error(e); else input->running=1;
  }
  CiukiAudioUnlock(); return r;
}
/* Called by the existing winmm worker under the audio guard, before dispatch.
 * At most 512 native frames/pass, no application callbacks under that guard. */
void wa_input_tick(void) {
  if (!input || !input->running) return;
  for (unsigned pass=0;pass<4;pass++) {
    if (input->count && input->queue[0]->recorded==input->queue[0]->bytes && !input_done()) return;
    struct ciuki_audio_capture_data c={.version=1,.size=sizeof(c),.frames=CIUKI_CAPTURE_FRAMES};
    int e=input_ctl(input->fd,AUDIO_CAPTURE_READ,&c,sizeof(c));
    if (e==-EAGAIN || e==-EINTR) return;
    if (e<0 || c.captured>CIUKI_CAPTURE_FRAMES) { input->failed=1; (void)input_stop(); return; }
    for (unsigned i=0;i<c.captured;i++) {
      input->phase+=input->format.rate;
      if (input->phase<48000) continue;
      input->phase-=48000;
      if (!input->count) continue; /* Windows drops input without queued buffers. */
      struct wa_header *h=input->queue[0];
      if (h->recorded==h->bytes) { if (!input_done()) return; if (!input->count) continue; h=input->queue[0]; }
      int32_t l=c.pcm[i*2],r=c.pcm[i*2+1];
      if (input->format.channels==1) l=(l+r)/2;
      for (unsigned ch=0;ch<input->format.channels;ch++) {
        int32_t sample=ch ? r : l;
        if (input->format.bits==16) { w32_put16(h->data+h->recorded,(uint16_t)sample); h->recorded+=2; }
        else h->data[h->recorded++]=(char)((sample+32768)/256);
      }
      input->frames++;
      if (h->recorded==h->bytes && !input_done()) return;
    }
  }
}
uint32_t API waveInStop(uintptr_t h) {
  CiukiAudioLock(); uint32_t r=0;
  if (!input || h!=(uintptr_t)input) r=5;
  else {
    wa_input_tick();
    if (input_stop()<0) r=6;
    if (input->count && input->queue[0]->recorded && !input_done()) r=WA_NOMEM;
  }
  CiukiAudioUnlock(); return r;
}
uint32_t API waveInReset(uintptr_t h) {
  CiukiAudioLock(); uint32_t r=0;
  if (!input || h!=(uintptr_t)input) r=5;
  else {
    if (input_stop()<0) r=6;
    while (input->count) if (!input_done()) { r=WA_NOMEM; break; }
    input->frames=input->phase=0;
  }
  CiukiAudioUnlock(); return r;
}
uint32_t API waveInClose(uintptr_t h) {
  CiukiAudioLock(); uint32_t r=0;
  if (!input || h!=(uintptr_t)input) r=5;
  else if (input->count) r=33;
  else {
    if (input_stop()<0) r=6;
    if (!input_event(0x3bf,0)) {
      input_close_fd(input->fd); wa_free(input); input=0;
    } else r=WA_NOMEM;
  }
  CiukiAudioUnlock(); return r;
}
uint32_t API waveInGetPosition(uintptr_t h, struct mmtime *p, uint32_t n) {
  CiukiAudioLock(); uint32_t r=0;
  if (!input || h!=(uintptr_t)input) r=5;
  else if (!p || n<sizeof(*p)) r=WA_INVALID;
  else {
    if (p->type==4) p->value[0]=input->frames*input->format.align;
    else if (p->type==2) p->value[0]=input->frames;
    else { p->type=1; p->value[0]=(uint32_t)((uint64_t)input->frames*1000/input->format.rate); }
    p->value[1]=0;
  }
  CiukiAudioUnlock(); return r;
}

/* Internal MCI route: same winmm instance, no new application message. */
void wa_wave_route(uintptr_t h, unsigned route) {
  CiukiAudioLock(); struct wave *w=get(h);
  if (w && route<3) w->source.route=route;
  CiukiAudioUnlock();
}

uint32_t API waveInGetID(uintptr_t h, uint32_t *id) {
  CiukiAudioLock();
  uint32_t r=!input || h!=(uintptr_t)input ? 5 : !id ? WA_INVALID : 0;
  if (!r) *id=0;
  CiukiAudioUnlock(); return r;
}
uint32_t API waveInMessage(uintptr_t h, uint32_t message, uintptr_t a, uintptr_t b) {
  (void)message; (void)a; (void)b;
  CiukiAudioLock(); uint32_t r=input && h==(uintptr_t)input ? 8 : 5;
  CiukiAudioUnlock(); return r;
}
static uint32_t input_error_text(uint32_t code,void *out,uint32_t cap,int wide) {
  if (!out || !cap) return WA_INVALID;
  const char *text;
  switch (code) {
  case 0: text="The operation completed successfully."; break;
  case 1: text="An unspecified error occurred."; break;
  case 2: text="The device identifier is out of range."; break;
  case 4: text="The recording device is already in use."; break;
  case 5: text="The waveform input handle is invalid."; break;
  case 6: text="No recording driver is available."; break;
  case 7: text="There is not enough memory for this operation."; break;
  case 8: text="The requested operation is not supported."; break;
  case 10: text="The flags are invalid."; break;
  case 11: text="A parameter is invalid."; break;
  case 32: text="The waveform format is not supported."; break;
  case 33: text="Recording buffers are still queued."; break;
  case 34: text="The waveform buffer has not been prepared."; break;
  default: return 9;
  }
  unsigned i=0;
  while (i+1<cap && text[i]) {
    if (wide) w32_put16((uint8_t *)out+i*2,(uint8_t)text[i]);
    else ((char *)out)[i]=text[i];
    i++;
  }
  if (wide) w32_put16((uint8_t *)out+i*2,0); else ((char *)out)[i]=0;
  return 0;
}
uint32_t API waveInGetErrorTextA(uint32_t code,char *out,uint32_t cap) { return input_error_text(code,out,cap,0); }
uint32_t API waveInGetErrorTextW(uint32_t code,uint16_t *out,uint32_t cap) { return input_error_text(code,out,cap,1); }
