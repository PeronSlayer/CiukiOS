/* MCI waveaudio device: RIFF WAVE files (PCM, and IMA/MS ADPCM decoded
 * through msacm32) streamed to this process's waveOut source in four
 * quarter-second headers refilled by the MCI service. Playback only.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "audio.h"
#include "mci.h"
#include <w32/kernel32.h>
extern HANDLE W32API LoadLibraryA(const char *);
extern void *W32API GetProcAddress(HANDLE, const char *);
uint32_t W32API waveOutOpen(uintptr_t *, uint32_t, const struct wa_format *,
                            uintptr_t, uintptr_t, uint32_t);
uint32_t W32API waveOutClose(uintptr_t);
uint32_t W32API waveOutPrepareHeader(uintptr_t, struct wa_header *, uint32_t);
uint32_t W32API waveOutUnprepareHeader(uintptr_t, struct wa_header *,
                                       uint32_t);
uint32_t W32API waveOutWrite(uintptr_t, struct wa_header *, uint32_t);
uint32_t W32API waveOutReset(uintptr_t);
uint32_t W32API waveOutPause(uintptr_t);
uint32_t W32API waveOutRestart(uintptr_t);
uint32_t W32API waveOutGetPosition(uintptr_t, void *, uint32_t);

/* msacm32 entry points, resolved on first compressed file. */
struct acm_header {
  uint32_t size, status;
  uintptr_t user;
  uint8_t *src;
  uint32_t src_len, src_used;
  uintptr_t src_user;
  uint8_t *dst;
  uint32_t dst_len, dst_used;
  uintptr_t dst_user;
  uint32_t reserved[10];
};
struct acm_api {
  uint32_t(W32API *open)(uintptr_t *, uintptr_t, const void *, const void *,
                         void *, uintptr_t, uintptr_t, uint32_t);
  uint32_t(W32API *close)(uintptr_t, uint32_t);
  uint32_t(W32API *prepare)(uintptr_t, struct acm_header *, uint32_t);
  uint32_t(W32API *unprepare)(uintptr_t, struct acm_header *, uint32_t);
  uint32_t(W32API *convert)(uintptr_t, struct acm_header *, uint32_t);
};
#ifdef W32_HOST_MODEL
const struct acm_api *mci_host_acm(void);
#endif
static const struct acm_api *acm(void) {
#ifdef W32_HOST_MODEL
  return mci_host_acm();
#else
  static struct acm_api api;
  static int state;
  if (!state) {
    HANDLE m = LoadLibraryA("msacm32.dll");
    state = 2;
    if (m) {
      api.open = GetProcAddress(m, "acmStreamOpen");
      api.close = GetProcAddress(m, "acmStreamClose");
      api.prepare = GetProcAddress(m, "acmStreamPrepareHeader");
      api.unprepare = GetProcAddress(m, "acmStreamUnprepareHeader");
      api.convert = GetProcAddress(m, "acmStreamConvert");
      if (api.open && api.close && api.prepare && api.unprepare &&
          api.convert)
        state = 1;
    }
  }
  return state == 1 ? &api : 0;
#endif
}

#define WAVE_BUFFERS 4u
#define FORMAT_MAX 1046u
struct wave {
  struct mci_cleanup cleanup;
  const struct mci_host *host;
  HANDLE file;
  uint32_t data_offset, data_bytes, total, time_format;
  uint8_t format[FORMAT_MAX];
  uint32_t tag, channels, rate, align, bits, spb; /* samples per block */
  struct wa_format pcm;
  uint32_t mode, position, from, to, read_at, skip, playing_serial;
  uintptr_t out, stream;
  struct wa_header header[WAVE_BUFFERS];
  uint8_t *buffer[WAVE_BUFFERS], *packed;
  uint32_t buffer_frames, packed_cap;
};
enum { STOPPED, PLAYING, PAUSED };

static int read_at(HANDLE f, uint32_t at, void *out, uint32_t n) {
  DWORD done = 0;
  if (SetFilePointer(f, (LONG)at, 0, 0) != at)
    return 0;
  return ReadFile(f, out, n, &done, 0) && done == n;
}
/* RIFF WAVE: "fmt " then "data", unknown chunks skipped, all bounded by
 * the RIFF size and the file size. */
static uint32_t parse(struct wave *w, uint32_t size) {
  uint8_t h[12];
  if (size < 12 || !read_at(w->file, 0, h, 12) || w32_u32(h) != 0x46464952u ||
      w32_u32(h + 8) != 0x45564157u)
    return MCIERR_INVALID_FILE;
  uint32_t end = w32_u32(h + 4);
  if (end > size - 8)
    end = size - 8; /* tolerate a RIFF size larger than the file */
  end += 8;
  uint32_t at = 12, fact = 0, have_fmt = 0, have_data = 0, fmt_len = 0;
  while (at + 8 <= end && !have_data) {
    uint32_t id, n;
    if (!read_at(w->file, at, h, 8))
      return MCIERR_FILE_READ;
    id = w32_u32(h);
    n = w32_u32(h + 4);
    if (n > end - at - 8) {
      if (id != 0x61746164u)
        return MCIERR_INVALID_FILE;
      n = end - at - 8; /* truncated data chunk: play what exists */
    }
    if (id == 0x20746d66u) {
      if (n < 16 || n > FORMAT_MAX)
        return MCIERR_INVALID_FILE;
      w32_fill(w->format, 0, sizeof(w->format));
      if (!read_at(w->file, at + 8, w->format, n))
        return MCIERR_FILE_READ;
      fmt_len = n;
      have_fmt = 1;
    } else if (id == 0x74636166u && n >= 4) {
      uint8_t f[4];
      if (!read_at(w->file, at + 8, f, 4))
        return MCIERR_FILE_READ;
      fact = w32_u32(f);
    } else if (id == 0x61746164u) {
      w->data_offset = at + 8;
      w->data_bytes = n;
      have_data = 1;
    }
    at += 8 + n + (n & 1);
  }
  if (!have_fmt || !have_data)
    return MCIERR_INVALID_FILE;
  w->tag = w32_u16(w->format);
  w->channels = w32_u16(w->format + 2);
  w->rate = w32_u32(w->format + 4);
  w->align = w32_u16(w->format + 12);
  w->bits = w32_u16(w->format + 14);
  if (!w->channels || !w->rate || !w->align)
    return MCIERR_INVALID_FILE;
  if (w->tag == 1) {
    if (w->align != w->channels * ((w->bits + 7) / 8))
      return MCIERR_INVALID_FILE;
    w->spb = 1;
    w->total = w->data_bytes / w->align;
    w->pcm = (struct wa_format){1, (uint16_t)w->channels, w->rate,
                                w->rate * w->align, (uint16_t)w->align,
                                (uint16_t)w->bits, 0};
  } else if ((w->tag == 2 || w->tag == 17) && fmt_len >= 20 &&
             w32_u16(w->format + 16) <= fmt_len - 18) {
    w->spb = w32_u16(w->format + 18);
    if (!w->spb)
      return MCIERR_INVALID_FILE;
    uint64_t total = (uint64_t)(w->data_bytes / w->align) * w->spb;
    if (fact && fact < total)
      total = fact;
    w->total = total > 0x7fffffffu ? 0x7fffffffu : (uint32_t)total;
    w->pcm = (struct wa_format){1,
                                (uint16_t)w->channels,
                                w->rate,
                                w->rate * w->channels * 2,
                                (uint16_t)(w->channels * 2),
                                16,
                                0};
  } else
    w->spb = 0; /* opened, but play reports an unsuitable format */
  return 0;
}
static uint32_t wave_open(struct mci_device *d, uint32_t flags,
                          struct mci_open *p) {
  (void)flags;
  (void)p;
  struct wave *w = d->host->alloc(sizeof(*w));
  if (!w)
    return MCIERR_OUT_OF_MEMORY;
  w32_fill(w, 0, sizeof(*w));
  w->host = d->host;
  w->file = K32_INVALID;
  if (d->element[0]) {
    w->file = CreateFileA(d->element, 0x80000000u, 1, 0, 3, 0, 0);
    if (w->file == K32_INVALID) {
      d->host->free(w);
      return MCIERR_FILE_NOT_FOUND;
    }
    DWORD high = 0, size = GetFileSize(w->file, &high);
    uint32_t e = high ? MCIERR_INVALID_FILE : parse(w, size);
    if (e) {
      CloseHandle(w->file);
      d->host->free(w);
      return e;
    }
  }
  d->data = w;
  return 0;
}
static int release_output(struct mci_device *d, struct wave *w) {
  (void)d;
  if (w->out) {
    if (waveOutReset(w->out)) return 0;
    for (unsigned i = 0; i < WAVE_BUFFERS; i++)
      if (w->header[i].flags & WA_PREPARED)
        if (waveOutUnprepareHeader(w->out, &w->header[i], sizeof(w->header[i]))) return 0;
    if (waveOutClose(w->out)) return 0;
    w->out = 0;
  }
  if (w->stream && acm() && acm()->close(w->stream, 0)) return 0;
  w->stream = 0;
  for (unsigned i = 0; i < WAVE_BUFFERS; i++) {
    w->host->free(w->buffer[i]);
    w->buffer[i] = 0;
  }
  w->host->free(w->packed);
  w->packed = 0;
  return 1;
}
static int wave_cleanup(void *p) {
  struct wave *w = p;
  if (!release_output(0, w)) return 0;
  w->host->free(w);
  return 1;
}
struct mmtime {
  uint32_t type;
  uint32_t value[2];
};
static uint32_t current(struct wave *w) {
  if (w->mode == STOPPED || !w->out)
    return w->position;
  struct mmtime t = {2, {0, 0}};
  if (waveOutGetPosition(w->out, &t, sizeof(t)) || t.type != 2)
    return w->from;
  uint32_t at = w->from + t.value[0];
  return at > w->to ? w->to : at;
}
static void halt(struct mci_device *d, struct wave *w) {
  if (w->mode != STOPPED) {
    w->position = current(w);
    release_output(d, w);
    w->mode = STOPPED;
  }
  if (d->busy)
    d->host->abort(d);
}
static void wave_close(struct mci_device *d) {
  struct wave *w = d->data;
  halt(d, w);
  if (w->file != K32_INVALID)
    CloseHandle(w->file);
  if (!wave_cleanup(w)) {
    w->cleanup.run = wave_cleanup;
    w->cleanup.context = w;
    d->host->defer(&w->cleanup);
  }
}
/* Fills one header with up to buffer_frames PCM frames from read_at. */
static uint32_t fill(struct mci_device *d, struct wave *w, unsigned i) {
  (void)d;
  struct wa_header *h = &w->header[i];
  uint32_t frames = 0, cap = w->buffer_frames;
  if (w->tag == 1) {
    uint32_t n = w->to - w->read_at;
    if (n > cap)
      n = cap;
    if (n && !read_at(w->file, w->data_offset + w->read_at * w->align,
                      w->buffer[i], n * w->align))
      return MCIERR_FILE_READ;
    frames = n;
    w->read_at += n;
  } else {
    /* Whole blocks; the first block after a seek skips its lead-in. */
    uint32_t out_align = w->channels * 2;
    while (frames + w->spb <= cap && w->read_at < w->to) {
      uint32_t block = w->read_at / w->spb;
      uint64_t offset = (uint64_t)block * w->align;
      if (offset + w->align > w->data_bytes)
        break;
      if (!read_at(w->file, w->data_offset + (uint32_t)offset, w->packed,
                   w->align))
        return MCIERR_FILE_READ;
      struct acm_header a = {0};
      a.size = sizeof(a);
      a.src = w->packed;
      a.src_len = w->align;
      a.dst = w->buffer[i] + frames * out_align;
      a.dst_len = (cap - frames) * out_align;
      if (acm()->prepare(w->stream, &a, 0))
        return MCIERR_DRIVER;
      uint32_t r = acm()->convert(w->stream, &a, 4);
      acm()->unprepare(w->stream, &a, 0);
      if (r)
        return MCIERR_INVALID_FILE;
      uint32_t got = a.dst_used / out_align, first = w->read_at % w->spb;
      if (got <= first)
        return MCIERR_INVALID_FILE;
      uint32_t keep = got - first;
      if (keep > w->to - w->read_at)
        keep = w->to - w->read_at;
      if (first)
        w32_copy(w->buffer[i] + frames * out_align,
                 w->buffer[i] + (frames + first) * out_align,
                 keep * out_align);
      frames += keep;
      w->read_at += keep;
    }
  }
  if (!frames)
    return 0;
  h->data = (char *)w->buffer[i];
  h->bytes = frames * w->pcm.align;
  h->flags = 0;
  h->loops = 0;
  if (waveOutPrepareHeader(w->out, h, sizeof(*h)) ||
      waveOutWrite(w->out, h, sizeof(*h)))
    return MCIERR_DRIVER;
  return 0;
}
static uint32_t start(struct mci_device *d, struct wave *w, uint32_t from,
                      uint32_t to) {
  if (!release_output(d, w)) return MCIERR_DEVICE_NOT_READY;
  if (!w->spb)
    return MCIERR_WAVE_OUTPUTSUNSUITABLE;
  uint32_t r = waveOutOpen(&w->out, 0, &w->pcm, 0, 0, 0);
  if (r)
    return r == WA_ALLOCATED ? MCIERR_WAVE_OUTPUTSINUSE
           : r == 32         ? MCIERR_WAVE_OUTPUTSUNSUITABLE
                             : MCIERR_DRIVER;
  if (w->tag != 1) {
    if (!acm() ||
        acm()->open(&w->stream, 0, w->format, &w->pcm, 0, 0, 0, 0)) {
      release_output(d, w);
      return MCIERR_WAVE_OUTPUTSUNSUITABLE;
    }
  }
  w->buffer_frames = w->rate / 4;
  if (w->tag != 1 && w->buffer_frames < w->spb)
    w->buffer_frames = w->spb;
  uint32_t bytes = w->buffer_frames * w->pcm.align;
  for (unsigned i = 0; i < WAVE_BUFFERS; i++) {
    w->buffer[i] = d->host->alloc(bytes);
    w32_fill(&w->header[i], 0, sizeof(w->header[i]));
  }
  w->packed = w->tag == 1 ? 0 : d->host->alloc(w->align);
  for (unsigned i = 0; i < WAVE_BUFFERS; i++)
    if (!w->buffer[i] || (w->tag != 1 && !w->packed)) {
      release_output(d, w);
      return MCIERR_OUT_OF_MEMORY;
    }
  w->from = from;
  w->to = to;
  w->read_at = from;
  w->mode = PLAYING;
  for (unsigned i = 0; i < WAVE_BUFFERS && w->read_at < w->to; i++) {
    r = fill(d, w, i);
    if (r) {
      release_output(d, w);
      w->mode = STOPPED;
      return r;
    }
  }
  d->host->begin(d);
  w->playing_serial = d->busy_serial;
  return 0;
}
static void wave_service(struct mci_device *d, uint32_t now) {
  (void)now;
  struct wave *w = d->data;
  if (w->mode == STOPPED) release_output(d, w);
  if (w->mode != PLAYING || !w->out)
    return;
  uint32_t outstanding = 0;
  for (unsigned i = 0; i < WAVE_BUFFERS; i++) {
    struct wa_header *h = &w->header[i];
    uint32_t f = __atomic_load_n(&h->flags, __ATOMIC_ACQUIRE);
    if ((f & WA_PREPARED) && !(f & WA_QUEUED)) {
      waveOutUnprepareHeader(w->out, h, sizeof(*h));
      h->flags = 0;
      f = 0;
    }
    if (!(f & WA_PREPARED) && w->read_at < w->to) {
      if (fill(d, w, i)) {
        w->position = current(w);
        release_output(d, w);
        w->mode = STOPPED;
        d->host->complete(d, MCI_NOTIFY_FAILURE);
        return;
      }
      f = h->flags;
    }
    if (f & WA_QUEUED)
      outstanding++;
  }
  if (!outstanding && w->read_at >= w->to) {
    release_output(d, w);
    w->mode = STOPPED;
    w->position = w->to;
    d->host->complete(d, MCI_NOTIFY_SUCCESSFUL);
  }
}
static uint32_t to_frames(struct wave *w, uint32_t v, uint32_t *out) {
  uint64_t f;
  if (w->time_format == MCI_FORMAT_BYTES)
    f = w->tag == 1 ? v / w->align
                    : (uint64_t)(v / (w->align ? w->align : 1)) * w->spb;
  else if (w->time_format == MCI_FORMAT_SAMPLES)
    f = v;
  else /* round up so that a position reads back as the same milliseconds */
    f = ((uint64_t)v * w->rate + 999) / 1000;
  if (f > w->total)
    return MCIERR_OUTOFRANGE;
  *out = (uint32_t)f;
  return 0;
}
static uint32_t from_frames(struct wave *w, uint32_t f) {
  if (w->time_format == MCI_FORMAT_BYTES)
    return w->tag == 1 ? f * w->align
                       : (w->spb ? f / w->spb * w->align : 0);
  if (w->time_format == MCI_FORMAT_SAMPLES)
    return f;
  return w->rate ? (uint32_t)((uint64_t)f * 1000 / w->rate) : 0;
}
static uint32_t boolean(int v) {
  return MCI_MAKERESOURCE(v ? 1 : 0, v ? MCI_TRUE : MCI_FALSE);
}
static uint32_t wave_command(struct mci_device *d, uint32_t msg,
                             uint32_t flags, void *parms) {
  struct wave *w = d->data;
  uint32_t e;
  switch (msg) {
  case MCI_PLAY: {
    struct mci_play *p = parms;
    uint32_t from = current(w), to = w->total;
    if ((flags & (MCI_FROM | MCI_TO)) && !p)
      return MCIERR_NULL_PARAMETER_BLOCK;
    if (w->file == K32_INVALID)
      return MCIERR_NONAPPLICABLE_FUNCTION;
    if ((flags & MCI_FROM) && (e = to_frames(w, p->from, &from)))
      return e;
    if ((flags & MCI_TO) && (e = to_frames(w, p->to, &to)))
      return e;
    if (from > to)
      return MCIERR_OUTOFRANGE;
    /* A play that keeps the end position continues the operation and its
     * pending notification (MM_MCINOTIFY remarks). */
    if (w->mode != STOPPED && !(flags & MCI_FROM) && to == w->to) {
      if (w->mode == PAUSED)
        waveOutRestart(w->out);
      w->mode = PLAYING;
      return MCI_ASYNC;
    }
    halt(d, w);
    w->position = from;
    if (from == to)
      return 0;
    e = start(d, w, from, to);
    return e ? e : MCI_ASYNC;
  }
  case MCI_STOP:
    halt(d, w);
    return 0;
  case MCI_PAUSE:
    if (w->mode == PLAYING) {
      waveOutPause(w->out);
      w->mode = PAUSED;
    }
    return 0;
  case MCI_RESUME:
    if (w->mode == PAUSED) {
      waveOutRestart(w->out);
      w->mode = PLAYING;
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
      if ((e = to_frames(w, p->to, &to)))
        return e;
    } else if (flags & MCI_SEEK_TO_END)
      to = w->total;
    halt(d, w);
    w->position = to;
    return 0;
  }
  case MCI_STATUS: {
    struct mci_status *p = parms;
    if (!p)
      return MCIERR_NULL_PARAMETER_BLOCK;
    if (!(flags & MCI_STATUS_ITEM))
      return MCIERR_MISSING_PARAMETER;
    if ((flags & MCI_TRACK) && p->track != 1)
      return MCIERR_OUTOFRANGE;
    uint32_t kind = 0;
    switch (p->item) {
    case MCI_STATUS_LENGTH:
      p->value = from_frames(w, w->total);
      break;
    case MCI_STATUS_POSITION:
      p->value = from_frames(w, (flags & MCI_STATUS_START) ? 0 : current(w));
      break;
    case MCI_STATUS_NUMBER_OF_TRACKS:
    case MCI_STATUS_CURRENT_TRACK:
      p->value = 1;
      break;
    case MCI_STATUS_MODE: {
      uint32_t m = w->mode == PLAYING  ? MCI_MODE_PLAY
                   : w->mode == PAUSED ? MCI_MODE_PAUSE
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
    case MCI_STATUS_TIME_FORMAT: {
      uint32_t f = w->time_format;
      p->value = MCI_MAKERESOURCE(f, MCI_FORMAT_RETURN_BASE + f);
      kind = MCI_RESOURCE_RETURNED;
      break;
    }
    case MCI_WAVE_STATUS_FORMATTAG:
      p->value = w->tag;
      if (w->tag == 1) {
        p->value = MCI_MAKERESOURCE(1, MCI_WAVE_PCM);
        kind = MCI_RESOURCE_RETURNED;
      }
      break;
    case MCI_WAVE_STATUS_CHANNELS:
      p->value = w->channels;
      break;
    case MCI_WAVE_STATUS_SAMPLESPERSEC:
      p->value = w->rate;
      break;
    case MCI_WAVE_STATUS_AVGBYTESPERSEC:
      p->value = w32_u32(w->format + 8);
      break;
    case MCI_WAVE_STATUS_BLOCKALIGN:
      p->value = w->align;
      break;
    case MCI_WAVE_STATUS_BITSPERSAMPLE:
      p->value = w->bits;
      break;
    case MCI_WAVE_STATUS_LEVEL:
      p->value = 0;
      break;
    case MCI_WAVE_INPUT:
      return MCIERR_WAVE_INPUTUNSPECIFIED;
    case MCI_WAVE_OUTPUT:
      p->value = 0;
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
    if (flags & (MCI_WAVE_SET_FORMATTAG | MCI_WAVE_SET_CHANNELS |
                 MCI_WAVE_SET_SAMPLESPERSEC | MCI_WAVE_SET_AVGBYTESPERSEC |
                 MCI_WAVE_SET_BLOCKALIGN | MCI_WAVE_SET_BITSPERSAMPLE))
      return MCIERR_NONAPPLICABLE_FUNCTION; /* only for recording */
    if (flags & (MCI_SET_AUDIO | MCI_SET_VIDEO | MCI_SET_DOOR_OPEN |
                 MCI_SET_DOOR_CLOSED | MCI_SET_ON | MCI_SET_OFF))
      return MCIERR_UNSUPPORTED_FUNCTION;
    if (flags & MCI_WAVE_INPUT)
      return MCIERR_WAVE_SETINPUTUNSUITABLE;
    if ((flags & MCI_WAVE_OUTPUT) && p->extra[1])
      return MCIERR_OUTOFRANGE;
    if (flags & MCI_SET_TIME_FORMAT) {
      if (p->time_format != MCI_FORMAT_MILLISECONDS &&
          p->time_format != MCI_FORMAT_BYTES &&
          p->time_format != MCI_FORMAT_SAMPLES)
        return MCIERR_BAD_TIME_FORMAT;
      w->time_format = p->time_format;
    }
    return 0;
  }
  case MCI_INFO: {
    struct mci_info *p = parms;
    const char *s;
    if (!p || !p->text)
      return MCIERR_NULL_PARAMETER_BLOCK;
    if (flags & MCI_INFO_PRODUCT)
      s = "CiukiOS waveform audio";
    else if (flags & MCI_INFO_FILE)
      s = d->element;
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
    case MCI_GETDEVCAPS_HAS_VIDEO:
    case MCI_GETDEVCAPS_CAN_EJECT:
    case MCI_GETDEVCAPS_CAN_SAVE:
      p->value = boolean(0);
      return MCI_RESOURCE_RETURNED;
    case MCI_GETDEVCAPS_HAS_AUDIO:
    case MCI_GETDEVCAPS_USES_FILES:
    case MCI_GETDEVCAPS_COMPOUND_DEVICE:
    case MCI_GETDEVCAPS_CAN_PLAY:
      p->value = boolean(1);
      return MCI_RESOURCE_RETURNED;
    case MCI_GETDEVCAPS_DEVICE_TYPE:
      p->value = MCI_MAKERESOURCE(MCI_DEVTYPE_WAVEFORM_AUDIO,
                                  MCI_DEVTYPE_WAVEFORM_AUDIO);
      return MCI_RESOURCE_RETURNED;
    case MCI_WAVE_GETDEVCAPS_INPUTS:
      p->value = 0;
      return 0;
    case MCI_WAVE_GETDEVCAPS_OUTPUTS:
      p->value = 1;
      return 0;
    }
    return MCIERR_UNSUPPORTED_FUNCTION;
  }
  case MCI_RECORD:
  case MCI_SAVE:
  case MCI_LOAD:
  case MCI_CUE:
    return MCIERR_UNSUPPORTED_FUNCTION;
  }
  return MCIERR_UNRECOGNIZED_COMMAND;
}
static const struct mci_driver driver = {MCI_BOUNDARY_VERSION,
                                         "waveaudio",
                                         MCI_DEVTYPE_WAVEFORM_AUDIO,
                                         wave_open,
                                         wave_command,
                                         wave_close,
                                         wave_service};
const struct mci_driver *mci_wave_driver(void) { return &driver; }
