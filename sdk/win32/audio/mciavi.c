/* mciavi32.dll: the MCI "avivideo" device (digital-video command set) over
 * avifil32 streams, msvfw32 DrawDib and msacm32 (f5-14C), drawing through
 * gdi32 into the default AVI window or a caller's window, and playing the
 * sound track through winmm waveOut into the f5-12A mixer. Loaded by the
 * winmm MCI core through CiukiMciDriver (private boundary, mci.h).
 * Commands and flags: Platform SDK digitalvideo command set (open, play,
 * pause, resume, stop, seek, step, status, set, info, capability, window,
 * put, where, update, realize). SPDX-License-Identifier: LGPL-2.1-or-later */
#include "audio.h"
#include "mci.h"
#ifndef W32_HOST_MODEL
#define IMPORT __declspec(dllimport)
#else
#define IMPORT
#endif
/* avifil32 */
IMPORT void W32API AVIFileInit(void);
IMPORT int32_t W32API AVIFileOpenA(void **, const char *, uint32_t, void *);
IMPORT uint32_t W32API AVIFileRelease(void *);
IMPORT int32_t W32API AVIFileGetStream(void *, void **, uint32_t, int32_t);
IMPORT uint32_t W32API AVIStreamRelease(void *);
IMPORT int32_t W32API AVIStreamInfoA(void *, void *, int32_t);
IMPORT int32_t W32API AVIStreamReadFormat(void *, int32_t, void *, int32_t *);
IMPORT int32_t W32API AVIStreamRead(void *, int32_t, int32_t, void *, int32_t,
                                    int32_t *, int32_t *);
IMPORT int32_t W32API AVIStreamStart(void *);
IMPORT int32_t W32API AVIStreamLength(void *);
IMPORT void *W32API AVIStreamGetFrameOpen(void *, void *);
IMPORT void *W32API AVIStreamGetFrame(void *, int32_t);
IMPORT int32_t W32API AVIStreamGetFrameClose(void *);
/* msvfw32 */
IMPORT uintptr_t W32API DrawDibOpen(void);
IMPORT int W32API DrawDibClose(uintptr_t);
IMPORT int W32API DrawDibDraw(uintptr_t, uintptr_t, int, int, int, int,
                              const void *, const void *, int, int, int, int,
                              uint32_t);
/* msacm32 */
IMPORT uint32_t W32API acmStreamOpen(uintptr_t *, uintptr_t, const void *,
                                     const void *, void *, uintptr_t,
                                     uintptr_t, uint32_t);
IMPORT uint32_t W32API acmStreamClose(uintptr_t, uint32_t);
IMPORT uint32_t W32API acmStreamPrepareHeader(uintptr_t, void *, uint32_t);
IMPORT uint32_t W32API acmStreamUnprepareHeader(uintptr_t, void *, uint32_t);
IMPORT uint32_t W32API acmStreamConvert(uintptr_t, void *, uint32_t);
/* winmm */
IMPORT uint32_t W32API waveOutOpen(uintptr_t *, uint32_t,
                                   const struct wa_format *, uintptr_t,
                                   uintptr_t, uint32_t);
IMPORT uint32_t W32API waveOutClose(uintptr_t);
IMPORT uint32_t W32API waveOutPrepareHeader(uintptr_t, struct wa_header *,
                                            uint32_t);
IMPORT uint32_t W32API waveOutUnprepareHeader(uintptr_t, struct wa_header *,
                                              uint32_t);
IMPORT uint32_t W32API waveOutWrite(uintptr_t, struct wa_header *, uint32_t);
IMPORT uint32_t W32API waveOutReset(uintptr_t);
IMPORT uint32_t W32API waveOutPause(uintptr_t);
IMPORT uint32_t W32API waveOutRestart(uintptr_t);
IMPORT uint32_t W32API waveOutGetPosition(uintptr_t, void *, uint32_t);
/* user32 / kernel32 */
struct avi_wndclass {
  uint32_t style;
  uintptr_t proc;
  int32_t cls_extra, wnd_extra;
  uintptr_t instance, icon, cursor, background, menu, name;
};
struct avi_paint {
  uintptr_t dc;
  int32_t erase, rect[4], restore, increment;
  uint8_t reserved[32];
};
IMPORT uint16_t W32API RegisterClassA(const struct avi_wndclass *);
IMPORT uintptr_t W32API CreateWindowExA(uint32_t, const char *, const char *,
                                        uint32_t, int, int, int, int,
                                        uintptr_t, uintptr_t, uintptr_t,
                                        void *);
IMPORT int W32API DestroyWindow(uintptr_t);
IMPORT int W32API ShowWindow(uintptr_t, int);
IMPORT uintptr_t W32API GetDC(uintptr_t);
IMPORT int W32API ReleaseDC(uintptr_t, uintptr_t);
IMPORT uintptr_t W32API BeginPaint(uintptr_t, struct avi_paint *);
IMPORT int W32API EndPaint(uintptr_t, const struct avi_paint *);
IMPORT intptr_t W32API DefWindowProcA(uintptr_t, uint32_t, uintptr_t,
                                      intptr_t);
IMPORT int W32API GetClientRect(uintptr_t, struct mci_rect *);
IMPORT int W32API GetWindowRect(uintptr_t, struct mci_rect *);
IMPORT int W32API SetWindowTextA(uintptr_t, const char *);
IMPORT int W32API SetWindowPos(uintptr_t, uintptr_t, int, int, int, int,
                               uint32_t);
IMPORT int W32API IsWindow(uintptr_t);
IMPORT int W32API IsWindowVisible(uintptr_t);
IMPORT int W32API GetSystemMetrics(int);
IMPORT int W32API AdjustWindowRect(struct mci_rect *, uint32_t, int);
IMPORT int W32API InvalidateRect(uintptr_t, const struct mci_rect *, int);
IMPORT int W32API PostMessageA(uintptr_t, uint32_t, uintptr_t, intptr_t);
IMPORT uint32_t W32API GetWindowThreadProcessId(uintptr_t, uint32_t *);
IMPORT uint32_t W32API GetCurrentThreadId(void);
IMPORT void *W32API GetModuleHandleA(const char *);
IMPORT uint32_t W32API GetStockObject(int);
IMPORT uint32_t W32API GetTickCount(void);

#define CLASS_NAME "CiukiAVIWindow"
#define AVI_BUFFERS 4u
#define AVI_CHUNKS 65536u
#define WS_OVERLAPPEDWINDOW 0x00CF0000u
#define WS_CHILD 0x40000000u
#define WS_POPUP 0x80000000u
#define WS_VISIBLE 0x10000000u
/* Private: restore a fullscreen window on its own thread (the MCI worker
 * must not size windows while it holds the MCI lock). */
#define WM_CIUKI_AVI_RESTORE 0x8001u
#define WM_CIUKI_AVI_DESTROY 0x8002u /* close on another thread */
enum { STOPPED, PLAYING, PAUSED };
struct avi {
  struct mci_cleanup cleanup;
  struct mci_device *device;
  void *file, *video, *audio, *getter;
  uint32_t frames, rate, scale, width, height, start_frame;
  uint32_t time_format, mode, position, from, to, repeat, skipped;
  uint32_t start_tick, pause_tick, drawn, video_on, audio_on, hidden;
  const void *image; /* last decoded frame (BITMAPINFOHEADER + bits) */
  uintptr_t window, own, drawdib;
  uint32_t own_style, fullscreen;
  struct mci_rect dest, source, saved_window, saved_dest;
  char title[128];
  /* audio */
  uint8_t aformat[1046];
  uint32_t atag, achannels, arate, aalign, aspb, achunks, astart, afixed;
  uint32_t *acum; /* cumulative samples before each chunk */
  uint32_t anext, askip, atotal, aopen;
  uint64_t aqueued;
  uint32_t aremaining;
  struct wa_format pcm;
  uintptr_t wave, acm;
  struct wa_header header[AVI_BUFFERS];
  uint8_t *buffer[AVI_BUFFERS], *packed;
  uint32_t buffer_bytes, packed_bytes;
};
static const struct mci_host *host;
#define AVI_DEVICES 32u
static struct avi *instances[AVI_DEVICES];
static uint16_t registered;

/* avifil32 frame getters return a 40-byte header, a full 256-entry colour
 * table for indexed formats, then the bits. */
static const uint8_t *frame_bits(const void *image) {
  const uint8_t *b = image;
  return b + 40 + (w32_u16(b + 14) <= 8 ? 1024 : 0);
}
static void draw(struct avi *a, uintptr_t dc) {
  if (!a->image || !a->video_on || !dc)
    return;
  DrawDibDraw(a->drawdib, dc, a->dest.left, a->dest.top, a->dest.right,
              a->dest.bottom, a->image, frame_bits(a->image), a->source.left,
              a->source.top, a->source.right, a->source.bottom, 0);
}
static int show_frame(struct avi *a, uint32_t frame) {
  if (frame >= a->frames)
    frame = a->frames ? a->frames - 1 : 0;
  const void *image = AVIStreamGetFrame(a->getter, (int32_t)(a->start_frame + frame));
  if (!image)
    return 0;
  a->image = image;
  a->drawn = frame;
  if (a->window) {
    uintptr_t dc = GetDC(a->window);
    draw(a, dc);
    if (dc)
      ReleaseDC(a->window, dc);
  }
  return 1;
}
static void redraw(struct avi *a) {
  if (!a->window)
    return;
  if (a->window == a->own)
    InvalidateRect(a->own, 0, 1); /* erase with the black class brush */
  uintptr_t dc = GetDC(a->window);
  draw(a, dc);
  if (dc)
    ReleaseDC(a->window, dc);
}
static struct avi *by_window(uintptr_t window) {
  for (unsigned i = 0; i < AVI_DEVICES; i++)
    if (instances[i] && instances[i]->own == window)
      return instances[i];
  return 0;
}
static void halt(struct mci_device *d, struct avi *a);
static void restore_fullscreen(struct avi *a);
static intptr_t W32API window_proc(uintptr_t window, uint32_t msg,
                                   uintptr_t wp, intptr_t lp) {
  if (msg == 0x000F) { /* WM_PAINT */
    struct avi_paint ps;
    uintptr_t dc = BeginPaint(window, &ps);
    host->lock();
    struct avi *a = by_window(window);
    if (a && a->window == window)
      draw(a, dc);
    host->unlock();
    EndPaint(window, &ps);
    return 0;
  }
  if (msg == WM_CIUKI_AVI_DESTROY) {
    DestroyWindow(window);
    return 0;
  }
  if (msg == WM_CIUKI_AVI_RESTORE) {
    host->lock();
    struct avi *a = by_window(window);
    if (a && a->mode == STOPPED)
      restore_fullscreen(a);
    host->unlock();
    return 0;
  }
  if (msg == 0x0010) { /* WM_CLOSE: stop the movie and hide its window */
    host->lock();
    struct avi *a = by_window(window);
    if (a && a->mode != STOPPED)
      halt(a->device, a);
    if (a)
      a->hidden = 1;
    host->unlock();
    ShowWindow(window, 0);
    return 0;
  }
  return DefWindowProcA(window, msg, wp, lp);
}
static uint32_t ms_of(struct avi *a, uint32_t frame) {
  return (uint32_t)((uint64_t)frame * a->scale * 1000 / a->rate);
}
static uint32_t frame_of(struct avi *a, uint32_t ms) {
  return (uint32_t)((uint64_t)ms * a->rate / ((uint64_t)a->scale * 1000));
}
static uint32_t to_frame(struct avi *a, uint32_t v, uint32_t *out) {
  uint64_t f = a->time_format == MCI_FORMAT_MILLISECONDS
                   ? ((uint64_t)v * a->rate + (uint64_t)a->scale * 1000 - 1) /
                         ((uint64_t)a->scale * 1000)
                   : v;
  if (!a->frames || f >= a->frames)
    return MCIERR_OUTOFRANGE;
  *out = (uint32_t)f;
  return 0;
}
static uint32_t from_frame(struct avi *a, uint32_t frame) {
  return a->time_format == MCI_FORMAT_MILLISECONDS ? ms_of(a, frame) : frame;
}

/* ---- audio ---- */
static int audio_release(struct avi *a) {
  if (a->wave) {
    if (waveOutReset(a->wave)) return 0;
    for (unsigned i = 0; i < AVI_BUFFERS; i++)
      if (a->header[i].flags & WA_PREPARED)
        if (waveOutUnprepareHeader(a->wave, &a->header[i], sizeof(a->header[i]))) return 0;
    if (waveOutClose(a->wave)) return 0;
    a->wave = 0;
  }
  if (a->acm && acmStreamClose(a->acm, 0)) return 0;
  a->acm = 0;
  for (unsigned i = 0; i < AVI_BUFFERS; i++) {
    host->free(a->buffer[i]);
    a->buffer[i] = 0;
  }
  host->free(a->packed);
  a->packed = 0;
  a->aopen = 0;
  return 1;
}
/* Decodes bounded whole sample/block batches (or variable-size chunks). */
static uint32_t audio_fill(struct avi *a, unsigned i) {
  uint32_t out = 0, align = a->pcm.align;
  while (a->anext < a->achunks && a->aremaining) {
    uint32_t count = 1;
    if (a->afixed) {
      count = (a->buffer_bytes - out) / (a->aspb * align);
      if (count > a->achunks - a->anext)
        count = a->achunks - a->anext;
      uint32_t blocks = (uint32_t)(((uint64_t)a->aremaining + a->askip + a->aspb - 1) / a->aspb);
      if (count > blocks) count = blocks;
      if (!count)
        break;
    }
    int32_t bytes = 0;
    if (AVIStreamRead(a->audio, (int32_t)(a->astart + a->anext), (int32_t)count, 0, 0,
                      &bytes, 0) ||
        bytes < 0)
      return MCIERR_FILE_READ;
    uint32_t decoded = a->atag == 1 ? (uint32_t)bytes
                                    : (uint32_t)bytes / a->aalign * a->aspb *
                                          align;
    if (out && out + decoded > a->buffer_bytes)
      break;
    if (decoded > a->buffer_bytes || (uint32_t)bytes > a->packed_bytes) {
      return MCIERR_INVALID_FILE; /* never silently discard an audio chunk */
    }
    uint8_t *target = a->buffer[i] + out;
    uint8_t *raw = a->atag == 1 ? target : a->packed;
    if (bytes && AVIStreamRead(a->audio, (int32_t)(a->astart + a->anext), (int32_t)count,
                               raw, bytes, 0, 0))
      return MCIERR_FILE_READ;
    uint32_t produced = (uint32_t)bytes;
    if (a->atag != 1 && bytes) {
      struct {
        uint32_t size, status;
        uintptr_t user;
        uint8_t *src;
        uint32_t src_len, src_used;
        uintptr_t src_user;
        uint8_t *dst;
        uint32_t dst_len, dst_used;
        uintptr_t dst_user;
        uint32_t reserved[10];
      } h = {0};
      h.size = sizeof(h);
      h.src = raw;
      h.src_len = (uint32_t)bytes;
      h.dst = target;
      h.dst_len = decoded;
      if (acmStreamPrepareHeader(a->acm, &h, 0))
        return MCIERR_DRIVER;
      uint32_t r = acmStreamConvert(a->acm, &h, 4);
      acmStreamUnprepareHeader(a->acm, &h, 0);
      if (r)
        return MCIERR_INVALID_FILE;
      produced = h.dst_used;
    }
    produced -= produced % align;
    if (a->askip) { /* first chunk after a seek: drop its lead-in */
      uint32_t skip = a->askip * align > produced ? produced : a->askip * align;
      w32_copy(target, target + skip, produced - skip);
      produced -= skip;
      a->askip = 0;
    }
    if (produced / align > a->aremaining) produced = a->aremaining * align;
    a->aremaining -= produced / align;
    out += produced;
    a->anext += count;
  }
  if (!out)
    return 0;
  struct wa_header *h = &a->header[i];
  w32_fill(h, 0, sizeof(*h));
  h->data = (char *)a->buffer[i];
  h->bytes = out;
  if (waveOutPrepareHeader(a->wave, h, sizeof(*h)) ||
      waveOutWrite(a->wave, h, sizeof(*h)))
    return MCIERR_DRIVER;
  a->aqueued += out / align;
  return 0;
}
static uint32_t audio_start(struct avi *a, uint32_t from_frame) {
  if (!a->audio || !a->audio_on || !a->achunks)
    return 0;
  uint64_t sample = (uint64_t)from_frame * a->scale * a->arate / a->rate;
  uint64_t end = ((uint64_t)a->to + 1) * a->scale * a->arate / a->rate;
  if (end > a->atotal) end = a->atotal;
  if (sample >= end) return 0;
  a->aremaining = (uint32_t)(end - sample);
  uint32_t chunk = a->afixed ? (uint32_t)sample / a->aspb : 0;
  if (!a->afixed)
    while (chunk + 1 < a->achunks && a->acum[chunk + 1] <= sample)
      chunk++;
  if (waveOutOpen(&a->wave, 0, &a->pcm, 0, 0, 0)) {
    return MCIERR_WAVE_OUTPUTSUNSUITABLE;
  }
  if (a->atag != 1 &&
      acmStreamOpen(&a->acm, 0, a->aformat, &a->pcm, 0, 0, 0, 0)) {
    audio_release(a);
    return MCIERR_WAVE_OUTPUTSUNSUITABLE;
  }
  a->buffer_bytes = a->arate / 4 * a->pcm.align;
  if (a->atag != 1) {
    uint32_t min = a->aspb * a->pcm.align * 2;
    if (a->buffer_bytes < min)
      a->buffer_bytes = min;
  }
  a->packed_bytes = a->buffer_bytes; /* compressed is never larger */
  for (unsigned i = 0; i < AVI_BUFFERS; i++)
    a->buffer[i] = host->alloc(a->buffer_bytes);
  a->packed = a->atag != 1 ? host->alloc(a->packed_bytes) : 0;
  for (unsigned i = 0; i < AVI_BUFFERS; i++)
    if (!a->buffer[i] || (a->atag != 1 && !a->packed)) {
      audio_release(a);
      return MCIERR_OUT_OF_MEMORY;
    }
  a->anext = chunk;
  a->askip = (uint32_t)sample - (a->afixed ? chunk * a->aspb : a->acum[chunk]);
  a->aqueued = 0;
  a->aopen = 1;
  for (unsigned i = 0; i < AVI_BUFFERS; i++) {
    uint32_t error = audio_fill(a, i);
    if (error) { audio_release(a); return error; }
  }
  return 0;
}
static uint32_t audio_service(struct avi *a) {
  if (!a->wave)
    return 0;
  for (unsigned i = 0; i < AVI_BUFFERS; i++) {
    struct wa_header *h = &a->header[i];
    uint32_t f = __atomic_load_n(&h->flags, __ATOMIC_ACQUIRE);
    if ((f & WA_PREPARED) && !(f & WA_QUEUED)) {
      waveOutUnprepareHeader(a->wave, h, sizeof(*h));
      h->flags = 0;
      uint32_t error = audio_fill(a, i);
      if (error) return error;
    }
  }
  return 0;
}

/* ---- device ---- */
static int window_class(void) {
  if (registered)
    return 1;
  struct avi_wndclass c = {0};
  c.proc = (uintptr_t)window_proc;
  c.instance = (uintptr_t)GetModuleHandleA("mciavi32.dll");
  c.name = (uintptr_t)CLASS_NAME;
  c.background = GetStockObject(4); /* BLACK_BRUSH */
  registered = RegisterClassA(&c);
  return registered != 0;
}
static uint32_t avi_open(struct mci_device *d, uint32_t flags,
                         struct mci_open *p) {
  if (!d->element[0])
    return MCIERR_FILENAME_REQUIRED;
  struct avi *a = host->alloc(sizeof(*a));
  if (!a)
    return MCIERR_OUT_OF_MEMORY;
  w32_fill(a, 0, sizeof(*a));
  a->video_on = a->audio_on = 1;
  a->time_format = MCI_FORMAT_FRAMES;
  a->device = d;
  AVIFileInit();
  uint32_t e = 0;
  int32_t r = AVIFileOpenA(&a->file, d->element, 0, 0);
  if (r) {
    host->free(a);
    return r == (int32_t)0x8004406Eu ? MCIERR_FILE_NOT_FOUND
                                     : MCIERR_INVALID_FILE;
  }
  uint8_t info[140];
  if (AVIFileGetStream(a->file, &a->video, 0x73646976u, 0) ||
      AVIStreamInfoA(a->video, info, sizeof(info)))
    e = MCIERR_INVALID_FILE;
  if (!e) {
    a->scale = w32_u32(info + 20);
    a->rate = w32_u32(info + 24);
    a->start_frame = (uint32_t)AVIStreamStart(a->video);
    int32_t length = AVIStreamLength(a->video);
    a->frames = length > 0 ? (uint32_t)length : 0;
    int32_t size = 0;
    if (!a->scale || !a->rate || !a->frames ||
        AVIStreamReadFormat(a->video, 0, 0, &size) || size < 40)
      e = MCIERR_INVALID_FILE;
    a->getter = e ? 0 : AVIStreamGetFrameOpen(a->video, 0);
    if (!e && !a->getter)
      e = MCIERR_INVALID_FILE; /* no installed decompressor */
  }
  if (!e) {
    const uint8_t *first = AVIStreamGetFrame(a->getter, (int32_t)a->start_frame);
    if (!first)
      e = MCIERR_INVALID_FILE;
    else {
      a->width = w32_u32(first + 4);
      int32_t h = (int32_t)w32_u32(first + 8);
      a->height = (uint32_t)(h < 0 ? -h : h);
      a->image = first;
    }
  }
  if (!e && !AVIFileGetStream(a->file, &a->audio, 0x73647561u, 0)) {
    /* Sound track: PCM or an ACM-decodable format; otherwise silent. */
    int32_t size = sizeof(a->aformat);
    uint8_t ainfo[140];
    if (AVIStreamReadFormat(a->audio, 0, a->aformat, &size) || size < 16 ||
        AVIStreamInfoA(a->audio, ainfo, sizeof(ainfo))) {
      AVIStreamRelease(a->audio);
      a->audio = 0;
    } else {
      a->atag = w32_u16(a->aformat);
      a->achannels = w32_u16(a->aformat + 2);
      a->arate = w32_u32(a->aformat + 4);
      a->aalign = w32_u16(a->aformat + 12);
      uint32_t bits = w32_u16(a->aformat + 14);
      a->aspb = a->atag == 1 ? 1 : (size >= 20 ? w32_u16(a->aformat + 18) : 0);
      a->astart = (uint32_t)AVIStreamStart(a->audio);
      a->afixed = w32_u32(ainfo + 44);
      int32_t chunks = AVIStreamLength(a->audio);
      int ok = a->achannels && a->achannels <= 2 && a->arate && a->aalign &&
               a->aspb && chunks > 0 &&
               (a->afixed ? a->afixed == a->aalign : (uint32_t)chunks <= AVI_CHUNKS) &&
               (a->atag == 1 || a->atag == 2 || a->atag == 17);
      if (ok) {
        a->pcm = (struct wa_format){1, (uint16_t)a->achannels, a->arate, 0,
                                    0, (uint16_t)(a->atag == 1 ? bits : 16),
                                    0};
        a->pcm.align = (uint16_t)(a->achannels * (a->pcm.bits / 8));
        a->pcm.average = a->arate * a->pcm.align;
        ok = a->pcm.align && (a->atag != 1 || a->aalign == a->pcm.align);
      }
      if (ok && a->afixed) {
        a->achunks = (uint32_t)chunks;
        ok = a->achunks <= 0xffffffffu / a->aspb;
        if (ok)
          a->atotal = a->achunks * a->aspb;
      } else if (ok) {
        a->achunks = (uint32_t)chunks;
        a->acum = host->alloc((a->achunks + 1) * 4);
        ok = a->acum != 0;
        for (uint32_t i = 0; ok && i < a->achunks; i++) {
          int32_t bytes = 0;
          if (AVIStreamRead(a->audio, (int32_t)(a->astart + i), 1, 0, 0,
                            &bytes, 0) ||
              bytes < 0)
            ok = 0;
          else {
            uint32_t n = a->atag == 1
                             ? (uint32_t)bytes / a->aalign
                             : (uint32_t)bytes / a->aalign * a->aspb;
            a->acum[i + 1] = a->acum[i] + n;
          }
        }
        if (ok)
          a->atotal = a->acum[a->achunks];
      }
      if (!ok) {
        host->free(a->acum);
        a->acum = 0;
        a->achunks = 0;
        AVIStreamRelease(a->audio);
        a->audio = 0;
      }
    }
  }
  if (!e) {
    a->drawdib = DrawDibOpen();
    if (!a->drawdib)
      e = MCIERR_OUT_OF_MEMORY;
  }
  a->dest = (struct mci_rect){0, 0, (int32_t)a->width, (int32_t)a->height};
  a->source = a->dest;
  if (!e) {
    /* Default window: hidden until play; a parent/style from "open". */
    uint32_t style = (flags & MCI_DGV_OPEN_WS) ? p->style : WS_OVERLAPPEDWINDOW;
    uintptr_t parent = (flags & MCI_DGV_OPEN_PARENT) ? p->parent : 0;
    struct mci_rect r = {0, 0, (int32_t)a->width, (int32_t)a->height};
    if (!(style & WS_CHILD))
      AdjustWindowRect(&r, style, 0);
    int x = (style & WS_CHILD) ? 0 : (int)0x80000000;
    a->own_style = style & ~WS_VISIBLE;
    a->own = window_class()
                 ? CreateWindowExA(0, CLASS_NAME, d->element, a->own_style, x,
                                   x, r.right - r.left, r.bottom - r.top,
                                   parent, 0,
                                   (uintptr_t)GetModuleHandleA("mciavi32.dll"),
                                   0)
                 : 0;
    if (!a->own)
      e = MCIERR_CREATEWINDOW;
    a->window = a->own;
  }
  if (e) {
    if (a->own)
      DestroyWindow(a->own);
    if (a->drawdib)
      DrawDibClose(a->drawdib);
    if (a->getter)
      AVIStreamGetFrameClose(a->getter);
    if (a->audio)
      AVIStreamRelease(a->audio);
    if (a->video)
      AVIStreamRelease(a->video);
    AVIFileRelease(a->file);
    host->free(a->acum);
    host->free(a);
    return e;
  }
  for (unsigned i = 0; i < AVI_DEVICES; i++)
    if (!instances[i]) {
      instances[i] = a;
      break;
    }
  d->data = a;
  return 0;
}
/* USER windows are thread-bound: sizing or destroying the movie window
 * from the MCI worker (auto-open completion) is posted to its thread. */
static int owner_thread(struct avi *a) {
  return GetWindowThreadProcessId(a->own, 0) == GetCurrentThreadId();
}
static void restore_fullscreen(struct avi *a) {
  if (!a->fullscreen)
    return;
  if (!owner_thread(a)) {
    PostMessageA(a->own, WM_CIUKI_AVI_RESTORE, 0, 0);
    return;
  }
  a->fullscreen = 0;
  a->dest = a->saved_dest;
  SetWindowPos(a->own, (uintptr_t)-2, a->saved_window.left,
               a->saved_window.top,
               a->saved_window.right - a->saved_window.left,
               a->saved_window.bottom - a->saved_window.top, 0x0010);
}
static void halt(struct mci_device *d, struct avi *a) {
  if (a->mode != STOPPED) {
    a->position = a->drawn;
    audio_release(a);
    a->mode = STOPPED;
    restore_fullscreen(a);
  }
  if (d->busy)
    host->abort(d);
}
static int avi_cleanup(void *p) {
  struct avi *a = p;
  if (!audio_release(a)) return 0;
  host->free(a);
  return 1;
}
static void avi_close(struct mci_device *d) {
  struct avi *a = d->data;
  halt(d, a);
  for (unsigned i = 0; i < AVI_DEVICES; i++)
    if (instances[i] == a)
      instances[i] = 0;
  if (a->own) {
    if (owner_thread(a))
      DestroyWindow(a->own);
    else
      PostMessageA(a->own, WM_CIUKI_AVI_DESTROY, 0, 0);
  }
  DrawDibClose(a->drawdib);
  AVIStreamGetFrameClose(a->getter);
  if (a->audio)
    AVIStreamRelease(a->audio);
  AVIStreamRelease(a->video);
  AVIFileRelease(a->file);
  host->free(a->acum);
  if (!avi_cleanup(a)) {
    a->cleanup.run = avi_cleanup;
    a->cleanup.context = a;
    host->defer(&a->cleanup);
  }
}
static uint32_t elapsed(struct avi *a) {
  uint32_t now = a->mode == PAUSED ? a->pause_tick : GetTickCount();
  return now - a->start_tick;
}
static void playback_failure(struct mci_device *d, struct avi *a, uint32_t error) {
  audio_release(a);
  a->mode = STOPPED;
  d->completion_error = error;
  restore_fullscreen(a);
  host->complete(d, MCI_NOTIFY_FAILURE);
}
static void avi_service(struct mci_device *d, uint32_t now) {
  (void)now;
  struct avi *a = d->data;
  if (a->mode == STOPPED) audio_release(a);
  if (a->mode != PLAYING) return;
  uint32_t error = audio_service(a);
  if (error) { playback_failure(d, a, error); return; }
  uint32_t ms = elapsed(a);
  uint32_t span = ms_of(a, a->to - a->from + 1);
  uint32_t target = a->from + frame_of(a, ms);
  if (target > a->to)
    target = a->to;
  if (target != a->drawn) {
    if (target > a->drawn + 1)
      a->skipped += target - a->drawn - 1;
    if (!show_frame(a, target)) { playback_failure(d, a, MCIERR_INVALID_FILE); return; }
  }
  if (ms >= span && a->wave && ms < span + 250) {
    /* Let the sound track drain (its tail is still in the mixer lead). */
    int busy = a->aremaining && a->anext < a->achunks;
    for (unsigned i = 0; i < AVI_BUFFERS && !busy; i++)
      busy = !!(__atomic_load_n(&a->header[i].flags, __ATOMIC_ACQUIRE) &
                WA_QUEUED);
    if (busy && !a->repeat)
      return;
  }
  if (ms >= span) {
    if (a->repeat) {
      a->start_tick = GetTickCount();
      if (!audio_release(a)) { playback_failure(d, a, MCIERR_DEVICE_NOT_READY); return; }
      error = show_frame(a, a->from) ? audio_start(a, a->from) : MCIERR_INVALID_FILE;
      if (error) playback_failure(d, a, error);
      return;
    }
    a->position = a->to;
    audio_release(a);
    a->mode = STOPPED;
    if (a->fullscreen)
      PostMessageA(a->own, WM_CIUKI_AVI_RESTORE, 0, 0);
    host->complete(d, MCI_NOTIFY_SUCCESSFUL);
  }
}
static uint32_t start(struct mci_device *d, struct avi *a, uint32_t from,
                      uint32_t to, uint32_t flags) {
  if (!audio_release(a)) return MCIERR_DEVICE_NOT_READY;
  a->from = from;
  a->to = to;
  a->repeat = !!(flags & MCI_DGV_PLAY_REPEAT);
  a->mode = PLAYING;
  a->skipped = 0;
  if ((flags & MCI_MCIAVI_PLAY_FULLSCREEN) && a->window == a->own) {
    /* Approximation: a top-level window covering the primary screen with
     * the movie scaled to fit, not a display-mode switch. */
    int cx = GetSystemMetrics(0), cy = GetSystemMetrics(1);
    if (!a->fullscreen) { /* a restore may still be pending: keep the
                             saved normal geometry */
      GetWindowRect(a->own, &a->saved_window);
      a->saved_dest = a->dest;
    }
    a->fullscreen = 1;
    uint32_t sx = (uint32_t)cx / a->width, sy = (uint32_t)cy / a->height;
    uint32_t k = sx < sy ? sx : sy;
    if (!k)
      k = 1;
    a->dest = (struct mci_rect){(cx - (int32_t)(a->width * k)) / 2,
                                (cy - (int32_t)(a->height * k)) / 2,
                                (int32_t)(a->width * k),
                                (int32_t)(a->height * k)};
    SetWindowPos(a->own, (uintptr_t)-1, 0, 0, cx, cy, 0x0040);
  } else if (a->window == a->own && !a->hidden)
    ShowWindow(a->own, 5);
  a->start_tick = GetTickCount();
  uint32_t error = show_frame(a, from) ? audio_start(a, from) : MCIERR_INVALID_FILE;
  if (error) { audio_release(a); a->mode = STOPPED; restore_fullscreen(a); return error; }
  host->begin(d);
  return MCI_ASYNC;
}
static uint32_t boolean(int v) {
  return MCI_MAKERESOURCE(v ? 1 : 0, v ? MCI_TRUE : MCI_FALSE);
}
static uint32_t avi_command(struct mci_device *d, uint32_t msg, uint32_t flags,
                            void *parms) {
  struct avi *a = d->data;
  uint32_t e;
  switch (msg) {
  case MCI_PLAY: {
    struct mci_play *p = parms;
    uint32_t from = a->mode == STOPPED ? a->position : a->drawn,
             to = a->frames - 1;
    if ((flags & (MCI_FROM | MCI_TO)) && !p)
      return MCIERR_NULL_PARAMETER_BLOCK;
    if (flags & MCI_DGV_PLAY_REVERSE)
      return MCIERR_UNSUPPORTED_FUNCTION;
    if ((flags & MCI_FROM) && (e = to_frame(a, p->from, &from)))
      return e;
    if ((flags & MCI_TO) && (e = to_frame(a, p->to, &to)))
      return e;
    if (from > to)
      return MCIERR_OUTOFRANGE;
    if (a->mode != STOPPED && !(flags & MCI_FROM) && to == a->to &&
        !(flags & (MCI_MCIAVI_PLAY_FULLSCREEN | MCI_DGV_PLAY_REPEAT))) {
      if (a->mode == PAUSED) {
        a->start_tick += GetTickCount() - a->pause_tick;
        if (a->wave)
          waveOutRestart(a->wave);
        a->mode = PLAYING;
      }
      return MCI_ASYNC; /* continues; keeps its notification */
    }
    halt(d, a);
    return start(d, a, from, to, flags);
  }
  case MCI_STOP:
    halt(d, a);
    return 0;
  case MCI_PAUSE:
    if (a->mode == PLAYING) {
      a->pause_tick = GetTickCount();
      if (a->wave)
        waveOutPause(a->wave);
      a->mode = PAUSED;
    }
    return 0;
  case MCI_RESUME:
    if (a->mode == PAUSED) {
      a->start_tick += GetTickCount() - a->pause_tick;
      if (a->wave)
        waveOutRestart(a->wave);
      a->mode = PLAYING;
    }
    return 0;
  case MCI_SEEK: {
    struct mci_seek *p = parms;
    uint32_t n = !!(flags & MCI_TO) + !!(flags & MCI_SEEK_TO_START) +
                 !!(flags & MCI_SEEK_TO_END),
             to = 0;
    if (n > 1)
      return MCIERR_FLAGS_NOT_COMPATIBLE;
    if (!n)
      return MCIERR_MISSING_PARAMETER;
    if (flags & MCI_TO) {
      if (!p)
        return MCIERR_NULL_PARAMETER_BLOCK;
      if ((e = to_frame(a, p->to, &to)))
        return e;
    } else if (flags & MCI_SEEK_TO_END)
      to = a->frames - 1;
    halt(d, a);
    a->position = to;
    show_frame(a, to);
    return 0;
  }
  case MCI_STEP: {
    struct mci_step *p = parms;
    uint32_t n = 1;
    if (flags & MCI_DGV_STEP_FRAMES) {
      if (!p)
        return MCIERR_NULL_PARAMETER_BLOCK;
      n = p->frames;
    }
    halt(d, a);
    uint32_t at = a->position;
    if (flags & MCI_DGV_STEP_REVERSE)
      at = n > at ? 0 : at - n;
    else
      at = n > a->frames - 1 - at ? a->frames - 1 : at + n;
    a->position = at;
    show_frame(a, at);
    return 0;
  }
  case MCI_STATUS: {
    struct mci_status *p = parms;
    uint32_t kind = 0;
    if (!p)
      return MCIERR_NULL_PARAMETER_BLOCK;
    if ((flags & MCI_TRACK) && p->track != 1)
      return MCIERR_OUTOFRANGE;
    switch (p->item) {
    case MCI_STATUS_LENGTH:
      p->value = a->time_format == MCI_FORMAT_MILLISECONDS
                     ? ms_of(a, a->frames)
                     : a->frames;
      break;
    case MCI_STATUS_POSITION:
      p->value = from_frame(a, (flags & MCI_STATUS_START) ? 0
                               : a->mode == STOPPED      ? a->position
                                                         : a->drawn);
      break;
    case MCI_STATUS_NUMBER_OF_TRACKS:
    case MCI_STATUS_CURRENT_TRACK:
      p->value = 1;
      break;
    case MCI_STATUS_MODE: {
      uint32_t m = a->mode == PLAYING  ? MCI_MODE_PLAY
                   : a->mode == PAUSED ? MCI_MODE_PAUSE
                                       : MCI_MODE_STOP;
      p->value = MCI_MAKERESOURCE(m, m);
      kind = MCI_RESOURCE_RETURNED;
      break;
    }
    case MCI_STATUS_MEDIA_PRESENT:
    case MCI_STATUS_READY:
      p->value = boolean(1);
      kind = MCI_RESOURCE_RETURNED;
      break;
    case MCI_DGV_STATUS_VIDEO:
      p->value = boolean((int)a->video_on);
      kind = MCI_RESOURCE_RETURNED;
      break;
    case MCI_DGV_STATUS_AUDIO:
      p->value = boolean(a->audio_on && a->audio);
      kind = MCI_RESOURCE_RETURNED;
      break;
    case MCI_STATUS_TIME_FORMAT:
      p->value = MCI_MAKERESOURCE(a->time_format,
                                  MCI_FORMAT_RETURN_BASE + a->time_format);
      kind = MCI_RESOURCE_RETURNED;
      break;
    case MCI_DGV_STATUS_HWND:
      p->value = a->window;
      break;
    case MCI_DGV_STATUS_SPEED:
      p->value = 1000;
      break;
    case MCI_DGV_STATUS_FRAME_RATE:
      p->value = (uint32_t)((uint64_t)a->rate * 1000 / a->scale);
      break;
    case MCI_DGV_STATUS_WINDOW_VISIBLE:
      p->value = boolean(IsWindowVisible(a->window));
      kind = MCI_RESOURCE_RETURNED;
      break;
    case MCI_DGV_STATUS_WINDOW_MINIMIZED:
    case MCI_DGV_STATUS_WINDOW_MAXIMIZED:
      p->value = boolean(0);
      kind = MCI_RESOURCE_RETURNED;
      break;
    case MCI_AVI_STATUS_FRAMES_SKIPPED:
      p->value = a->skipped;
      break;
    default:
      return MCIERR_UNSUPPORTED_FUNCTION;
    }
    return kind;
  }
  case MCI_SET: {
    struct mci_set *p = parms;
    if (!p)
      return MCIERR_NULL_PARAMETER_BLOCK;
    if (flags & (MCI_SET_DOOR_OPEN | MCI_SET_DOOR_CLOSED))
      return MCIERR_UNSUPPORTED_FUNCTION;
    if ((flags & MCI_DGV_SET_SPEED) && p->extra[1] != 1000)
      return MCIERR_UNSUPPORTED_FUNCTION;
    if ((flags & (MCI_SET_AUDIO | MCI_SET_VIDEO)) &&
        !(flags & (MCI_SET_ON | MCI_SET_OFF)))
      return MCIERR_MISSING_PARAMETER;
    if ((flags & MCI_SET_ON) && (flags & MCI_SET_OFF))
      return MCIERR_FLAGS_NOT_COMPATIBLE;
    if (flags & MCI_SET_TIME_FORMAT) {
      if (p->time_format != MCI_FORMAT_FRAMES &&
          p->time_format != MCI_FORMAT_MILLISECONDS)
        return MCIERR_BAD_TIME_FORMAT;
      a->time_format = p->time_format;
    }
    if (flags & MCI_SET_AUDIO) {
      if (p->audio != 0)
        return MCIERR_UNSUPPORTED_FUNCTION; /* left/right: all only */
      a->audio_on = !!(flags & MCI_SET_ON);
      if (!a->audio_on)
        audio_release(a);
    }
    if (flags & MCI_SET_VIDEO)
      a->video_on = !!(flags & MCI_SET_ON);
    return 0;
  }
  case MCI_INFO: {
    struct mci_info *p = parms;
    const char *s;
    if (!p || !p->text)
      return MCIERR_NULL_PARAMETER_BLOCK;
    if (flags & MCI_INFO_PRODUCT)
      s = "CiukiOS AVI video";
    else if (flags & MCI_INFO_FILE)
      s = d->element;
    else if (flags & MCI_DGV_INFO_TEXT)
      s = a->title[0] ? a->title : d->element;
    else
      return MCIERR_UNSUPPORTED_FUNCTION;
    if (w32_strlen(s) >= p->size)
      return MCIERR_PARAM_OVERFLOW;
    w32_copy((char *)p->text, s, w32_strlen(s) + 1);
    return 0;
  }
  case MCI_GETDEVCAPS: {
    struct mci_caps *p = parms;
    if (!p)
      return MCIERR_NULL_PARAMETER_BLOCK;
    switch (p->item) {
    case MCI_GETDEVCAPS_CAN_RECORD:
    case MCI_GETDEVCAPS_CAN_EJECT:
    case MCI_GETDEVCAPS_CAN_SAVE:
    case MCI_DGV_GETDEVCAPS_CAN_LOCK:
    case MCI_DGV_GETDEVCAPS_CAN_FREEZE:
    case MCI_DGV_GETDEVCAPS_CAN_REVERSE:
    case MCI_DGV_GETDEVCAPS_HAS_STILL:
      p->value = boolean(0);
      return MCI_RESOURCE_RETURNED;
    case MCI_GETDEVCAPS_HAS_AUDIO:
      p->value = boolean(a->audio != 0);
      return MCI_RESOURCE_RETURNED;
    case MCI_GETDEVCAPS_HAS_VIDEO:
    case MCI_GETDEVCAPS_USES_FILES:
    case MCI_GETDEVCAPS_COMPOUND_DEVICE:
    case MCI_GETDEVCAPS_CAN_PLAY:
    case MCI_DGV_GETDEVCAPS_CAN_STRETCH:
      p->value = boolean(1);
      return MCI_RESOURCE_RETURNED;
    case MCI_GETDEVCAPS_DEVICE_TYPE:
      p->value = MCI_MAKERESOURCE(MCI_DEVTYPE_DIGITAL_VIDEO,
                                  MCI_DEVTYPE_DIGITAL_VIDEO);
      return MCI_RESOURCE_RETURNED;
    case MCI_DGV_GETDEVCAPS_MAX_WINDOWS:
      p->value = 1;
      return 0;
    case MCI_DGV_GETDEVCAPS_MAXIMUM_RATE:
    case MCI_DGV_GETDEVCAPS_MINIMUM_RATE:
      p->value = 1000;
      return 0;
    }
    return MCIERR_UNSUPPORTED_FUNCTION;
  }
  case MCI_WINDOW: {
    struct mci_window *p = parms;
    if (!p)
      return MCIERR_NULL_PARAMETER_BLOCK;
    if (flags & MCI_DGV_WINDOW_HWND) {
      uintptr_t w = p->window ? p->window : a->own;
      if (p->window && !IsWindow(p->window))
        return MCIERR_NO_WINDOW;
      if (w != a->own && a->window == a->own)
        ShowWindow(a->own, 0);
      a->window = w;
      if (a->window == a->own && a->mode != STOPPED && !a->hidden)
        ShowWindow(a->own, 5);
      redraw(a);
    }
    if (flags & MCI_DGV_WINDOW_STATE) {
      ShowWindow(a->window, (int)p->show);
      if (a->window == a->own)
        a->hidden = p->show == 0;
    }
    if (flags & MCI_DGV_WINDOW_TEXT) {
      const char *t = (const char *)p->text;
      if (!t)
        return MCIERR_MISSING_STRING_ARGUMENT;
      uint32_t n = w32_strlen(t);
      if (n >= sizeof(a->title))
        return MCIERR_PARAM_OVERFLOW;
      w32_copy(a->title, t, n + 1);
      SetWindowTextA(a->own, a->title);
    }
    return 0;
  }
  case MCI_PUT: {
    struct mci_put *p = parms;
    if (!p)
      return MCIERR_NULL_PARAMETER_BLOCK;
    struct mci_rect r = p->rect;
    int rect = !!(flags & MCI_DGV_RECT);
    if (rect && (r.right <= 0 || r.bottom <= 0))
      return MCIERR_OUTOFRANGE;
    if (flags & MCI_DGV_PUT_DESTINATION)
      a->dest = rect ? r
                     : (struct mci_rect){0, 0, (int32_t)a->width,
                                         (int32_t)a->height};
    if (flags & MCI_DGV_PUT_SOURCE) {
      if (rect && ((uint32_t)r.left + (uint32_t)r.right > a->width ||
                   (uint32_t)r.top + (uint32_t)r.bottom > a->height ||
                   r.left < 0 || r.top < 0))
        return MCIERR_OUTOFRANGE;
      a->source = rect ? r
                       : (struct mci_rect){0, 0, (int32_t)a->width,
                                           (int32_t)a->height};
    }
    if ((flags & MCI_DGV_PUT_WINDOW) && rect)
      SetWindowPos(a->own, 0, r.left, r.top, r.right, r.bottom, 0x0014);
    if (flags & (MCI_DGV_PUT_FRAME | MCI_DGV_PUT_VIDEO))
      return MCIERR_UNSUPPORTED_FUNCTION;
    redraw(a);
    return 0;
  }
  case MCI_WHERE: {
    struct mci_put *p = parms;
    if (!p)
      return MCIERR_NULL_PARAMETER_BLOCK;
    if (flags & MCI_DGV_WHERE_SOURCE)
      p->rect = (flags & MCI_DGV_WHERE_MAX)
                    ? (struct mci_rect){0, 0, (int32_t)a->width,
                                        (int32_t)a->height}
                    : a->source;
    else if (flags & MCI_DGV_WHERE_DESTINATION) {
      if (flags & MCI_DGV_WHERE_MAX) {
        struct mci_rect c;
        GetClientRect(a->window, &c);
        p->rect = (struct mci_rect){0, 0, c.right - c.left, c.bottom - c.top};
      } else
        p->rect = a->dest;
    } else if (flags & MCI_DGV_WHERE_WINDOW) {
      struct mci_rect w;
      GetWindowRect(a->window, &w);
      p->rect = (flags & MCI_DGV_WHERE_MAX)
                    ? (struct mci_rect){0, 0, GetSystemMetrics(0),
                                        GetSystemMetrics(1)}
                    : (struct mci_rect){w.left, w.top, w.right - w.left,
                                        w.bottom - w.top};
    } else
      return MCIERR_MISSING_PARAMETER;
    return MCI_RECT_RETURNED;
  }
  case MCI_UPDATE: {
    struct mci_update *p = parms;
    if (!p || !(flags & MCI_DGV_UPDATE_HDC))
      return MCIERR_MISSING_PARAMETER;
    struct mci_rect saved = a->dest;
    if (flags & MCI_DGV_RECT)
      a->dest = p->rect;
    draw(a, p->dc);
    a->dest = saved;
    return 0;
  }
  case MCI_REALIZE:
    return 0;
  case MCI_RECORD:
  case MCI_SAVE:
  case MCI_LOAD:
  case MCI_CUE:
  case MCI_FREEZE:
  case MCI_UNFREEZE:
  case MCI_CONFIGURE:
    return MCIERR_UNSUPPORTED_FUNCTION;
  }
  return MCIERR_UNRECOGNIZED_COMMAND;
}
static const struct mci_driver driver = {
    MCI_BOUNDARY_VERSION, "avivideo", MCI_DEVTYPE_DIGITAL_VIDEO, avi_open,
    avi_command,          avi_close,  avi_service};
const struct mci_driver *W32API CiukiMciDriver(const struct mci_host *h) {
  if (!h || h->version != MCI_BOUNDARY_VERSION)
    return 0;
  host = h;
  return &driver;
}
