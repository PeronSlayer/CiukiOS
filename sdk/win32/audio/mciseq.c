/* MCI sequencer device: Standard MIDI Files (format 0/1, optional RIFF
 * RMID wrapper) parsed with bounds, merged per track on the fly, timed by a
 * tempo map (PPQN) or the SMPTE division, and played through this process's
 * midiStream (f5-12A) as 1-microsecond ticks. Pause re-chases controllers
 * and programs on resume. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "audio.h"
#include "mci.h"
#include <w32/kernel32.h>
uint32_t W32API midiStreamOpen(uintptr_t *, uint32_t *, uint32_t, uintptr_t,
                               uintptr_t, uint32_t);
uint32_t W32API midiStreamClose(uintptr_t);
uint32_t W32API midiStreamProperty(uintptr_t, uint32_t *, uint32_t);
uint32_t W32API midiStreamOut(uintptr_t, void *, uint32_t);
uint32_t W32API midiStreamRestart(uintptr_t);
uint32_t W32API midiStreamStop(uintptr_t);
uint32_t W32API midiOutPrepareHeader(uintptr_t, void *, uint32_t);
uint32_t W32API midiOutUnprepareHeader(uintptr_t, void *, uint32_t);
extern struct wa_engine *wa_process_engine(void);

#define SEQ_FILE_MAX (1024u * 1024u)
#define SEQ_TRACKS 128u
#define SEQ_TEMPOS 2048u
#define SEQ_BUFFERS 4u
#define SEQ_BUFFER_BYTES 4096u
#define SEQ_SYSEX_MAX 1024u
struct seq_header { /* MIDIHDR, i386 layout */
  char *data;
  uint32_t bytes, recorded;
  uintptr_t user;
  uint32_t flags;
  struct seq_header *next;
  uintptr_t reserved;
  uint32_t offset;
  uintptr_t extra[8];
};
struct track {
  uint32_t pos, end;
  uint64_t tick;
  uint8_t status, done;
};
struct tempo {
  uint64_t tick, us;
  uint32_t value;
};
struct event {
  uint64_t tick;
  uint32_t length; /* bytes of a short message, or sysex length */
  const uint8_t *data;
  uint8_t kind; /* 0 short, 1 sysex (F0 prefixed), 2 escape, 3 tempo, 4 end */
  uint8_t message[3];
};
struct seq {
  uint8_t *file;
  uint32_t size, format, tracks, division;
  struct track start[SEQ_TRACKS], cursor[SEQ_TRACKS];
  struct tempo tempo[SEQ_TEMPOS];
  uint32_t tempos;
  uint64_t end_tick, end_us;
  uint32_t time_format, mode, port;
  uint64_t position_us, from_us, to_us, last_us, start_frame;
  uint32_t rate, closing, exhausted, chase_pending;
  uintptr_t stream;
  struct seq_header header[SEQ_BUFFERS];
  uint8_t buffer[SEQ_BUFFERS][SEQ_BUFFER_BYTES];
  /* chase state */
  uint8_t program[16], control[16][128], bend[16][2], pressure[16];
  uint8_t seen_program[16], seen_control[16][128], seen_bend[16],
      seen_pressure[16];
  uint32_t chase_at;
  struct event held;
  uint32_t holding;
};
enum { STOPPED, PLAYING, PAUSED };

static uint32_t be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
         p[3];
}
static int varlen(const uint8_t *p, uint32_t *pos, uint32_t end,
                  uint32_t *out) {
  uint32_t v = 0;
  for (unsigned i = 0; i < 4; i++) {
    if (*pos >= end)
      return 0;
    uint8_t b = p[(*pos)++];
    v = v << 7 | (b & 127);
    if (!(b & 128)) {
      *out = v;
      return 1;
    }
  }
  return 0;
}
/* Reads the delta time of the next event of a track. */
static int track_delta(struct seq *s, struct track *t) {
  uint32_t delta;
  if (t->pos >= t->end) {
    t->done = 1;
    return 1;
  }
  if (!varlen(s->file, &t->pos, t->end, &delta))
    return 0;
  t->tick += delta;
  if (t->tick > 0xffffffffull) /* keeps tick*tempo below 2^56 */
    return 0;
  return 1;
}
/* Parses the event at the cursor (after its delta); 0 on malformed data. */
static int track_event(struct seq *s, struct track *t, struct event *e) {
  const uint8_t *p = s->file;
  uint8_t status;
  e->tick = t->tick;
  e->kind = 0;
  if (t->pos >= t->end)
    return 0;
  if (p[t->pos] & 0x80)
    status = p[t->pos++];
  else if (t->status)
    status = t->status;
  else
    return 0;
  if (status < 0xf0) {
    uint32_t n = (status & 0xe0) == 0xc0 ? 1 : 2;
    if (t->end - t->pos < n)
      return 0;
    t->status = status;
    e->message[0] = status;
    e->message[1] = p[t->pos] & 127;
    e->message[2] = n == 2 ? p[t->pos + 1] & 127 : 0;
    e->length = n + 1;
    t->pos += n;
    return 1;
  }
  t->status = 0; /* system messages cancel running status */
  uint32_t length;
  if (status == 0xff) {
    if (t->pos >= t->end)
      return 0;
    uint8_t type = p[t->pos++];
    if (!varlen(p, &t->pos, t->end, &length) || length > t->end - t->pos)
      return 0;
    e->data = p + t->pos;
    e->length = length;
    t->pos += length;
    if (type == 0x2f) {
      e->kind = 4;
      t->pos = t->end;
    } else if (type == 0x51 && length == 3)
      e->kind = 3;
    else
      e->kind = 5; /* other meta: ignored */
    return 1;
  }
  if (status == 0xf0 || status == 0xf7) {
    if (!varlen(p, &t->pos, t->end, &length) || length > t->end - t->pos)
      return 0;
    e->data = p + t->pos;
    e->length = length;
    e->kind = status == 0xf0 ? 1 : 2;
    t->pos += length;
    return 1;
  }
  return 0; /* F1-FE are not valid file events */
}
/* Next merged event: lowest tick, then lowest track number. */
static int next_event(struct seq *s, struct event *e, uint32_t *which) {
  uint32_t best = SEQ_TRACKS;
  for (uint32_t i = 0; i < s->tracks; i++)
    if (!s->cursor[i].done &&
        (best == SEQ_TRACKS || s->cursor[i].tick < s->cursor[best].tick))
      best = i;
  if (best == SEQ_TRACKS)
    return 0;
  struct track *t = &s->cursor[best];
  if (!track_event(s, t, e) || !track_delta(s, t))
    return -1;
  if (e->kind == 4)
    t->done = 1;
  if (which)
    *which = best;
  return 1;
}
static void rewind_tracks(struct seq *s) {
  for (uint32_t i = 0; i < s->tracks; i++)
    s->cursor[i] = s->start[i];
}
static uint64_t tick_to_us(const struct seq *s, uint64_t tick) {
  if (s->division & 0x8000) {
    uint32_t fps = (uint32_t)-(int8_t)(s->division >> 8),
             sub = s->division & 255;
    if (fps == 29)
      return tick * 1001000ull / (30ull * sub);
    return tick * 1000000ull / ((uint64_t)fps * sub);
  }
  uint32_t i = 0;
  while (i + 1 < s->tempos && s->tempo[i + 1].tick <= tick)
    i++;
  return s->tempo[i].us +
         (tick - s->tempo[i].tick) * s->tempo[i].value / s->division;
}
static uint64_t us_to_tick(const struct seq *s, uint64_t us) {
  if (s->division & 0x8000) {
    uint32_t fps = (uint32_t)-(int8_t)(s->division >> 8),
             sub = s->division & 255;
    if (fps == 29)
      return us * 30ull * sub / 1001000ull;
    return us * fps * sub / 1000000ull;
  }
  uint32_t i = 0;
  while (i + 1 < s->tempos && s->tempo[i + 1].us <= us)
    i++;
  return s->tempo[i].tick +
         (us - s->tempo[i].us) * s->division / s->tempo[i].value;
}
static uint32_t load(struct seq *s, HANDLE f) {
  DWORD high = 0, size = GetFileSize(f, &high), done = 0;
  if (high || size < 14 || size > SEQ_FILE_MAX)
    return MCIERR_INVALID_FILE;
  s->file = wa_alloc(size);
  if (!s->file)
    return MCIERR_OUT_OF_MEMORY;
  if (SetFilePointer(f, 0, 0, 0) != 0 ||
      !ReadFile(f, s->file, size, &done, 0) || done != size)
    return MCIERR_FILE_READ;
  s->size = size;
  uint32_t at = 0, end = size;
  const uint8_t *p = s->file;
  if (w32_u32(p) == 0x46464952u && size >= 20 &&
      w32_u32(p + 8) == 0x44494d52u) { /* RIFF RMID: find "data" */
    uint32_t limit = w32_u32(p + 4);
    limit = limit > size - 8 ? size : limit + 8;
    at = 12;
    while (1) {
      if (at + 8 > limit)
        return MCIERR_INVALID_FILE;
      uint32_t n = w32_u32(p + at + 4);
      if (n > limit - at - 8)
        return MCIERR_INVALID_FILE;
      if (w32_u32(p + at) == 0x61746164u) {
        at += 8;
        end = at + n;
        break;
      }
      at += 8 + n + (n & 1);
    }
  }
  if (end - at < 14 || be32(p + at) != 0x4d546864u)
    return MCIERR_INVALID_FILE;
  uint32_t hlen = be32(p + at + 4);
  if (hlen < 6 || hlen > end - at - 8)
    return MCIERR_INVALID_FILE;
  s->format = (uint32_t)p[at + 8] << 8 | p[at + 9];
  uint32_t declared = (uint32_t)p[at + 10] << 8 | p[at + 11];
  s->division = (uint32_t)p[at + 12] << 8 | p[at + 13];
  /* A format 0 file has one track whatever its header declares (fx-classaudit). */
  if (s->format == 0 && declared != 1) {
    OutputDebugStringA("[midi] field=format0_track_count action=use_one\n");
    declared = 1;
  }
  if (s->format > 1 || !declared || declared > SEQ_TRACKS || !s->division)
    return MCIERR_INVALID_FILE;
  if (s->division & 0x8000) {
    int fps = -(int8_t)(s->division >> 8);
    if ((fps != 24 && fps != 25 && fps != 29 && fps != 30) ||
        !(s->division & 255))
      return MCIERR_INVALID_FILE;
  }
  at += 8 + hlen;
  while (s->tracks < declared && end - at >= 8) {
    uint32_t id = be32(p + at), n = be32(p + at + 4);
    if (n > end - at - 8)
      return MCIERR_INVALID_FILE;
    if (id == 0x4d54726bu) { /* MTrk; other chunk types are skipped */
      struct track *t = &s->start[s->tracks++];
      t->pos = at + 8;
      t->end = at + 8 + n;
      t->tick = 0;
      t->status = 0;
      t->done = 0;
      if (!track_delta(s, t))
        return MCIERR_INVALID_FILE;
    }
    at += 8 + n;
  }
  if (s->tracks != declared)
    return MCIERR_INVALID_FILE;
  /* First pass: validate every event, build the tempo map, find the end. */
  s->tempo[0] = (struct tempo){0, 0, 500000};
  s->tempos = 1;
  rewind_tracks(s);
  struct event e;
  int r;
  uint32_t budget = 4000000;
  while ((r = next_event(s, &e, 0)) > 0) {
    if (!budget--)
      return MCIERR_INVALID_FILE;
    if (e.tick > s->end_tick)
      s->end_tick = e.tick;
    if (e.kind == 3 && !(s->division & 0x8000)) {
      uint32_t value = (uint32_t)e.data[0] << 16 | e.data[1] << 8 | e.data[2];
      if (!value)
        return MCIERR_INVALID_FILE;
      struct tempo *last = &s->tempo[s->tempos - 1];
      uint64_t us = last->us + (e.tick - last->tick) * last->value /
                                   s->division;
      if (e.tick == last->tick)
        last->value = value;
      else if (s->tempos == SEQ_TEMPOS)
        return MCIERR_INVALID_FILE;
      else
        s->tempo[s->tempos++] = (struct tempo){e.tick, us, value};
    }
  }
  if (r < 0)
    return MCIERR_INVALID_FILE;
  s->end_us = tick_to_us(s, s->end_tick);
  if (s->end_us > 0xffffffffull * 1000)
    return MCIERR_INVALID_FILE;
  return 0;
}
static uint32_t seq_open(struct mci_device *d, uint32_t flags,
                         struct mci_open *p) {
  (void)flags;
  (void)p;
  if (!d->element[0])
    return MCIERR_FILENAME_REQUIRED;
  HANDLE f = CreateFileA(d->element, 0x80000000u, 1, 0, 3, 0, 0);
  if (f == K32_INVALID)
    return MCIERR_FILE_NOT_FOUND;
  struct seq *s = d->host->alloc(sizeof(*s));
  if (!s) {
    CloseHandle(f);
    return MCIERR_OUT_OF_MEMORY;
  }
  w32_fill(s, 0, sizeof(*s));
  uint32_t e = load(s, f);
  CloseHandle(f);
  if (e) {
    d->host->free(s->file);
    d->host->free(s);
    return e;
  }
  /* PPQN files default to milliseconds; SMPTE files to their frame rate
   * ("the sequence file sets the default format", Platform SDK set). */
  if (s->division & 0x8000) {
    int fps = -(int8_t)(s->division >> 8);
    s->time_format = fps == 24   ? MCI_FORMAT_SMPTE_24
                     : fps == 25 ? MCI_FORMAT_SMPTE_25
                     : fps == 29 ? MCI_FORMAT_SMPTE_30DROP
                                 : MCI_FORMAT_SMPTE_30;
  }
  s->port = MCI_SEQ_MAPPER;
  d->data = s;
  return 0;
}
static uint64_t current_us(struct seq *s) {
  if (s->mode != PLAYING || !s->stream)
    return s->position_us;
  CiukiAudioLock();
  struct wa_engine *e = wa_process_engine();
  uint64_t consumed = e->consumed;
  uint32_t rate = e->rate ? e->rate : 44100;
  CiukiAudioUnlock();
  uint64_t at = s->from_us;
  if (consumed > s->start_frame)
    at += (consumed - s->start_frame) * 1000000ull / rate;
  return at > s->to_us ? s->to_us : at;
}
static void close_stream(struct seq *s) {
  if (!s->stream)
    return;
  for (unsigned i = 0; i < SEQ_BUFFERS; i++)
    if (s->header[i].flags & 2)
      midiOutUnprepareHeader(s->stream, &s->header[i], sizeof(s->header[i]));
  if (midiStreamClose(s->stream))
    s->closing = 1; /* completion records still queued: retry in service */
  else {
    s->stream = 0;
    s->closing = 0;
  }
}
static void silence(struct seq *s) {
  if (s->stream) {
    midiStreamStop(s->stream);
    close_stream(s);
  }
}
static void halt(struct mci_device *d, struct seq *s, int abort) {
  if (s->mode == PLAYING)
    s->position_us = current_us(s);
  if (s->mode != STOPPED)
    silence(s);
  s->mode = STOPPED;
  if (abort && d->busy)
    d->host->abort(d);
}
static void seq_close(struct mci_device *d) {
  struct seq *s = d->data;
  halt(d, s, 1);
  if (s->stream) {
    /* The mixer worker retires the reset records; never leak the port. */
    for (unsigned i = 0; i < 200 && midiStreamClose(s->stream); i++)
      Sleep(5);
  }
  d->host->free(s->file);
  d->host->free(s);
}
static void chase_record(struct seq *s, const struct event *e) {
  if (e->kind)
    return;
  uint8_t ch = e->message[0] & 15, type = e->message[0] & 0xf0;
  if (type == 0xb0) {
    s->control[ch][e->message[1]] = e->message[2];
    s->seen_control[ch][e->message[1]] = 1;
  } else if (type == 0xc0) {
    s->program[ch] = e->message[1];
    s->seen_program[ch] = 1;
  } else if (type == 0xe0) {
    s->bend[ch][0] = e->message[1];
    s->bend[ch][1] = e->message[2];
    s->seen_bend[ch] = 1;
  } else if (type == 0xd0) {
    s->pressure[ch] = e->message[1];
    s->seen_pressure[ch] = 1;
  }
}
static int put(uint8_t *b, uint32_t *at, uint32_t delta, uint32_t event,
               const uint8_t *data, uint32_t n) {
  uint32_t need = 12 + ((n + 3) & ~3u);
  if (*at + need > SEQ_BUFFER_BYTES)
    return 0;
  w32_put32(b + *at, delta);
  w32_put32(b + *at + 4, 0);
  w32_put32(b + *at + 8, event);
  if (n) {
    w32_fill(b + *at + 12, 0, (n + 3) & ~3u);
    w32_copy(b + *at + 12, data, n);
  }
  *at += need;
  return 1;
}
/* Emits chase state (delta 0) then merged events up to `to`. */
static uint32_t fill(struct seq *s, unsigned i) {
  uint8_t *b = s->buffer[i];
  uint32_t at = 0;
  while (s->chase_pending && s->chase_at < 16 * 132) {
    uint32_t ch = s->chase_at / 132, item = s->chase_at % 132, msg = 0;
    if (item < 128) {
      if (s->seen_control[ch][item])
        msg = (0xb0u | ch) | item << 8 | (uint32_t)s->control[ch][item] << 16;
    } else if (item == 128 && s->seen_program[ch])
      msg = (0xc0u | ch) | (uint32_t)s->program[ch] << 8;
    else if (item == 129 && s->seen_bend[ch])
      msg = (0xe0u | ch) | (uint32_t)s->bend[ch][0] << 8 |
            (uint32_t)s->bend[ch][1] << 16;
    else if (item == 130 && s->seen_pressure[ch])
      msg = (0xd0u | ch) | (uint32_t)s->pressure[ch] << 8;
    if (msg && !put(b, &at, 0, msg, 0, 0))
      break;
    s->chase_at++;
  }
  if (s->chase_at >= 16 * 132)
    s->chase_pending = 0;
  while (!s->chase_pending && !s->exhausted) {
    struct event e;
    if (s->holding)
      e = s->held;
    else {
      int r = next_event(s, &e, 0);
      if (r <= 0) {
        s->exhausted = 1;
        break;
      }
    }
    uint64_t us = tick_to_us(s, e.tick);
    if (us > s->to_us) { /* events at the end position still play */
      s->exhausted = 1;
      s->holding = 0;
      break;
    }
    uint64_t delta = us > s->last_us ? us - s->last_us : 0;
    int ok = 1;
    if (e.kind == 0)
      ok = put(b, &at, (uint32_t)delta,
               e.message[0] | (uint32_t)e.message[1] << 8 |
                   (uint32_t)e.message[2] << 16,
               0, 0);
    else if ((e.kind == 1 || e.kind == 2) && e.length < SEQ_SYSEX_MAX) {
      uint8_t sysex[SEQ_SYSEX_MAX + 1];
      uint32_t n = 0;
      if (e.kind == 1)
        sysex[n++] = 0xf0;
      w32_copy(sysex + n, e.data, e.length);
      n += e.length;
      ok = n ? put(b, &at, (uint32_t)delta, 0x80000000u | n, sysex, n) : 1;
    } else
      ok = delta ? put(b, &at, (uint32_t)delta, 0x02000000u, 0, 0) : 1;
    if (!ok) {
      s->held = e;
      s->holding = 1;
      break;
    }
    s->holding = 0;
    s->last_us = us > s->last_us ? us : s->last_us;
  }
  if (!at)
    return 0;
  struct seq_header *h = &s->header[i];
  w32_fill(h, 0, sizeof(*h));
  h->data = (char *)b;
  h->bytes = h->recorded = at;
  if (midiOutPrepareHeader(s->stream, h, sizeof(*h)) ||
      midiStreamOut(s->stream, h, sizeof(*h)))
    return MCIERR_SEQ_PORT_MISCERROR;
  return 0;
}
static uint32_t start(struct seq *s, uint64_t from_us, uint64_t to_us) {
  uint32_t r, device = 0;
  /* "port none" disables output; a sequence without a port is not played
   * (no silent clock is provided). */
  if (s->port == MCI_SEQ_NONE)
    return MCIERR_SEQ_PORTUNSPECIFIED;
  if (!s->stream) {
    r = midiStreamOpen(&s->stream, &device, 1, 0, 0, 0);
    if (r) {
      s->stream = 0;
      return r == 4 ? MCIERR_SEQ_PORT_INUSE
             : r == 6 ? MCIERR_SEQ_NOMIDIPRESENT
                      : MCIERR_SEQ_PORT_MISCERROR;
    }
  }
  s->closing = 0;
  uint32_t prop[2] = {8, 1000};
  if (midiStreamProperty(s->stream, prop, 0x80000001u) ||
      (prop[1] = 1000, midiStreamProperty(s->stream, prop, 0x80000002u)) ||
      midiStreamRestart(s->stream)) {
    close_stream(s);
    return MCIERR_SEQ_PORT_MISCERROR;
  }
  /* Chase: replay controller/program state of everything before `from`. */
  w32_fill(s->seen_control, 0, sizeof(s->seen_control));
  w32_fill(s->seen_program, 0, sizeof(s->seen_program));
  w32_fill(s->seen_bend, 0, sizeof(s->seen_bend));
  w32_fill(s->seen_pressure, 0, sizeof(s->seen_pressure));
  rewind_tracks(s);
  s->holding = 0;
  s->exhausted = 0;
  uint64_t from_tick = us_to_tick(s, from_us);
  for (;;) {
    uint32_t best = SEQ_TRACKS;
    for (uint32_t i = 0; i < s->tracks; i++)
      if (!s->cursor[i].done &&
          (best == SEQ_TRACKS || s->cursor[i].tick < s->cursor[best].tick))
        best = i;
    if (best == SEQ_TRACKS || s->cursor[best].tick >= from_tick)
      break;
    struct event e;
    if (next_event(s, &e, 0) <= 0)
      break;
    chase_record(s, &e);
  }
  s->chase_pending = from_us > 0;
  s->chase_at = 0;
  s->from_us = from_us;
  s->to_us = to_us;
  s->last_us = from_us;
  CiukiAudioLock();
  struct wa_engine *e = wa_process_engine();
  s->start_frame = e->rendered;
  s->rate = e->rate;
  CiukiAudioUnlock();
  for (unsigned i = 0; i < SEQ_BUFFERS; i++)
    if ((r = fill(s, i))) {
      silence(s);
      return r;
    }
  s->mode = PLAYING;
  return 0;
}
static void seq_service(struct mci_device *d, uint32_t now) {
  (void)now;
  struct seq *s = d->data;
  if (s->closing && s->mode != PLAYING && s->stream && !midiStreamClose(s->stream)) {
    s->stream = 0;
    s->closing = 0;
  }
  if (s->mode != PLAYING || !s->stream)
    return;
  uint32_t outstanding = 0;
  for (unsigned i = 0; i < SEQ_BUFFERS; i++) {
    struct seq_header *h = &s->header[i];
    uint32_t f = __atomic_load_n(&h->flags, __ATOMIC_ACQUIRE);
    if ((f & 2) && (f & 1) && !(f & 4)) {
      midiOutUnprepareHeader(s->stream, h, sizeof(*h));
      h->flags = 0;
      f = 0;
      if (fill(s, i)) {
        halt(d, s, 0);
        d->host->complete(d, MCI_NOTIFY_FAILURE);
        return;
      }
      f = h->flags;
    }
    if (f & 4)
      outstanding++;
  }
  if (!outstanding && s->exhausted && !s->holding &&
      current_us(s) >= s->to_us) {
    s->position_us = s->to_us;
    silence(s);
    s->mode = STOPPED;
    d->host->complete(d, MCI_NOTIFY_SUCCESSFUL);
  }
}
/* Time format conversions: ms, song pointer (PPQN sixteenth notes), SMPTE.
 */
static uint32_t from_us(struct seq *s, uint64_t us, uint32_t *kind) {
  *kind = 0;
  if (s->time_format == MCI_SEQ_FORMAT_SONGPTR)
    return (uint32_t)(us_to_tick(s, us) * 4 / s->division);
  return mci_ms_to_format((uint32_t)(us / 1000), s->time_format, kind);
}
static uint32_t to_us(struct seq *s, uint32_t v, uint64_t *out) {
  uint32_t ms;
  if (s->time_format == MCI_SEQ_FORMAT_SONGPTR) {
    uint64_t tick = (uint64_t)v * s->division / 4;
    if (tick > s->end_tick)
      return MCIERR_OUTOFRANGE;
    *out = tick_to_us(s, tick);
    return 0;
  }
  if (mci_format_to_ms(v, s->time_format, &ms))
    return MCIERR_OUTOFRANGE;
  if ((uint64_t)ms * 1000 > s->end_us)
    return MCIERR_OUTOFRANGE;
  *out = (uint64_t)ms * 1000;
  return 0;
}
static uint32_t boolean(int v) {
  return MCI_MAKERESOURCE(v ? 1 : 0, v ? MCI_TRUE : MCI_FALSE);
}
static uint32_t seq_command(struct mci_device *d, uint32_t msg, uint32_t flags,
                            void *parms) {
  struct seq *s = d->data;
  uint32_t e, kind;
  switch (msg) {
  case MCI_PLAY: {
    struct mci_play *p = parms;
    uint64_t from = current_us(s), to = s->end_us;
    if ((flags & (MCI_FROM | MCI_TO)) && !p)
      return MCIERR_NULL_PARAMETER_BLOCK;
    if ((flags & MCI_FROM) && (e = to_us(s, p->from, &from)))
      return e;
    if ((flags & MCI_TO) && (e = to_us(s, p->to, &to)))
      return e;
    if (from > to)
      return MCIERR_OUTOFRANGE;
    if (s->mode != STOPPED && !(flags & MCI_FROM) && to == s->to_us) {
      if (s->mode == PAUSED) {
        e = start(s, s->position_us, s->to_us);
        if (e) {
          s->mode = STOPPED;
          if (d->busy)
            d->host->abort(d);
          return e;
        }
      }
      return MCI_ASYNC;
    }
    halt(d, s, 1);
    s->position_us = from;
    if (from == to)
      return 0;
    if ((e = start(s, from, to)))
      return e;
    d->host->begin(d);
    return MCI_ASYNC;
  }
  case MCI_STOP:
    halt(d, s, 1);
    return 0;
  case MCI_PAUSE:
    if (s->mode == PLAYING) {
      s->position_us = current_us(s);
      silence(s);
      s->mode = PAUSED;
    }
    return 0;
  case MCI_RESUME:
    if (s->mode == PAUSED) {
      e = start(s, s->position_us, s->to_us);
      if (e) {
        s->mode = STOPPED;
        if (d->busy)
          d->host->complete(d, MCI_NOTIFY_FAILURE);
        return e;
      }
    }
    return 0;
  case MCI_SEEK: {
    struct mci_seek *p = parms;
    uint32_t n = !!(flags & MCI_TO) + !!(flags & MCI_SEEK_TO_START) +
                 !!(flags & MCI_SEEK_TO_END);
    uint64_t to = 0;
    if (n > 1)
      return MCIERR_FLAGS_NOT_COMPATIBLE;
    if (!n)
      return MCIERR_MISSING_PARAMETER;
    if (flags & MCI_TO) {
      if (!p)
        return MCIERR_NULL_PARAMETER_BLOCK;
      if ((e = to_us(s, p->to, &to)))
        return e;
    } else if (flags & MCI_SEEK_TO_END)
      to = s->end_us;
    halt(d, s, 1);
    s->position_us = to;
    return 0;
  }
  case MCI_STATUS: {
    struct mci_status *p = parms;
    if (!p)
      return MCIERR_NULL_PARAMETER_BLOCK;
    if ((flags & MCI_TRACK) && p->track != 1)
      return MCIERR_OUTOFRANGE;
    kind = 0;
    switch (p->item) {
    case MCI_STATUS_LENGTH:
      p->value = from_us(s, s->end_us, &kind);
      break;
    case MCI_STATUS_POSITION:
      p->value =
          from_us(s, (flags & MCI_STATUS_START) ? 0 : current_us(s), &kind);
      break;
    case MCI_STATUS_NUMBER_OF_TRACKS:
    case MCI_STATUS_CURRENT_TRACK:
      p->value = 1;
      break;
    case MCI_STATUS_MODE: {
      uint32_t m = s->mode == PLAYING  ? MCI_MODE_PLAY
                   : s->mode == PAUSED ? MCI_MODE_PAUSE
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
      uint32_t f = s->time_format;
      p->value = f == MCI_SEQ_FORMAT_SONGPTR
                     ? MCI_MAKERESOURCE(f, MCI_SEQ_SONGPTR_S)
                     : MCI_MAKERESOURCE(f, MCI_FORMAT_RETURN_BASE + f);
      kind = MCI_RESOURCE_RETURNED;
      break;
    }
    case MCI_SEQ_STATUS_TEMPO: {
      if (s->division & 0x8000) {
        int fps = -(int8_t)(s->division >> 8);
        p->value = (uint32_t)(fps == 29 ? 30 : fps);
      } else {
        uint64_t tick = us_to_tick(s, current_us(s));
        uint32_t i = 0;
        while (i + 1 < s->tempos && s->tempo[i + 1].tick <= tick)
          i++;
        p->value = (60000000u + s->tempo[i].value / 2) / s->tempo[i].value;
      }
      break;
    }
    case MCI_SEQ_STATUS_PORT:
      p->value = s->port == MCI_SEQ_MAPPER
                     ? MCI_MAKERESOURCE(MCI_SEQ_MAPPER, MCI_SEQ_MAPPER_S)
                 : s->port == MCI_SEQ_NONE
                     ? MCI_MAKERESOURCE(MCI_SEQ_NONE, MCI_SEQ_NONE_S)
                     : s->port;
      kind = s->port >= MCI_SEQ_NONE ? MCI_RESOURCE_RETURNED : 0;
      break;
    case MCI_SEQ_STATUS_SLAVE:
      p->value = MCI_SEQ_FILE;
      break;
    case MCI_SEQ_STATUS_MASTER:
      p->value = MCI_SEQ_NONE;
      break;
    case MCI_SEQ_STATUS_OFFSET:
      p->value = 0;
      kind = MCI_COLONIZED4_RETURN;
      break;
    case MCI_SEQ_STATUS_DIVTYPE: {
      uint32_t t = MCI_SEQ_DIV_PPQN;
      if (s->division & 0x8000) {
        int fps = -(int8_t)(s->division >> 8);
        t = fps == 24   ? MCI_SEQ_DIV_SMPTE_24
            : fps == 25 ? MCI_SEQ_DIV_SMPTE_25
            : fps == 29 ? MCI_SEQ_DIV_SMPTE_30DROP
                        : MCI_SEQ_DIV_SMPTE_30;
      }
      p->value = MCI_MAKERESOURCE(t, t);
      kind = MCI_RESOURCE_RETURNED;
      break;
    }
    default:
      return MCIERR_UNSUPPORTED_FUNCTION;
    }
    return kind;
  }
  case MCI_SET: {
    struct mci_set *p = parms;
    if (!p)
      return MCIERR_NULL_PARAMETER_BLOCK;
    if (flags & (MCI_SET_AUDIO | MCI_SET_VIDEO | MCI_SET_DOOR_OPEN |
                 MCI_SET_DOOR_CLOSED | MCI_SET_ON | MCI_SET_OFF |
                 MCI_SEQ_SET_MASTER | MCI_SEQ_SET_OFFSET | MCI_SEQ_SET_TEMPO))
      return MCIERR_UNSUPPORTED_FUNCTION;
    if ((flags & MCI_SEQ_SET_SLAVE) && p->extra[2] != MCI_SEQ_FILE)
      return MCIERR_UNSUPPORTED_FUNCTION;
    if (flags & MCI_SEQ_SET_PORT) {
      uint32_t port = p->extra[1];
      if (port != 0 && port != MCI_SEQ_MAPPER && port != MCI_SEQ_NONE)
        return MCIERR_SEQ_PORT_NONEXISTENT;
      if (port == MCI_SEQ_NONE)
        halt(d, s, 1);
      s->port = port;
    }
    if (flags & MCI_SET_TIME_FORMAT) {
      uint32_t f = p->time_format;
      if (f == MCI_SEQ_FORMAT_SONGPTR) {
        if (s->division & 0x8000)
          return MCIERR_SEQ_DIV_INCOMPATIBLE;
      } else if (f != MCI_FORMAT_MILLISECONDS && f != MCI_FORMAT_SMPTE_24 &&
                 f != MCI_FORMAT_SMPTE_25 && f != MCI_FORMAT_SMPTE_30 &&
                 f != MCI_FORMAT_SMPTE_30DROP)
        return MCIERR_BAD_TIME_FORMAT;
      s->time_format = f;
    }
    return 0;
  }
  case MCI_INFO: {
    struct mci_info *p = parms;
    const char *text;
    if (!p || !p->text)
      return MCIERR_NULL_PARAMETER_BLOCK;
    if (flags & MCI_INFO_PRODUCT)
      text = "CiukiOS MIDI sequencer";
    else if (flags & MCI_INFO_FILE)
      text = d->element;
    else
      return MCIERR_UNSUPPORTED_FUNCTION;
    if (w32_strlen(text) >= p->size)
      return MCIERR_PARAM_OVERFLOW;
    w32_copy((char *)p->text, text, w32_strlen(text) + 1);
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
      p->value =
          MCI_MAKERESOURCE(MCI_DEVTYPE_SEQUENCER, MCI_DEVTYPE_SEQUENCER);
      return MCI_RESOURCE_RETURNED;
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
static const struct mci_driver driver = {
    MCI_BOUNDARY_VERSION, "sequencer", MCI_DEVTYPE_SEQUENCER, seq_open,
    seq_command,          seq_close,   seq_service};
const struct mci_driver *mci_seq_driver(void) { return &driver; }
