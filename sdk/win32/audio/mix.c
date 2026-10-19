/* Integer PCM renderer. No SSE/MMX, device calls or wide division in frame
 * loop. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "audio.h"
#include "gain.h"
static uint32_t min(uint32_t a, uint32_t b) { return a < b ? a : b; }
void wa_init(struct wa_engine *e, const struct wa_transport *t) {
  w32_fill(e, 0, sizeof(*e));
  e->transport = *t;
  e->rate = 44100;
  e->focused = 1;
  e->volume[0] = e->volume[1] = 65535;
}
int wa_valid_format(const struct wa_format *f) {
  return f && f->tag == 1 && (f->channels == 1 || f->channels == 2) &&
         (f->bits == 8 || f->bits == 16) && f->rate >= 4000 &&
         f->rate <= 48000 && f->align == f->channels * f->bits / 8 &&
         f->average == f->rate * f->align;
}
uint32_t wa_open(struct wa_engine *e, uint32_t rate) {
  if (e->opened)
    return 0;
  e->rate = rate == 22050 || rate == 48000 ? rate : 44100;
  int r = e->transport.open(e->transport.opaque, e->rate);
  if (r < 0)
    return r == -12 ? WA_ALLOCATED : 6;
  uint64_t n;
  uint32_t g;
  if (e->transport.status(e->transport.opaque, &n, &g) < 0) {
    e->transport.close(e->transport.opaque);
    return 6;
  }
  e->opened = 1;
  e->base_consumed = n;
  e->generation = g;
  return 0;
}
void wa_close(struct wa_engine *e) {
  if (e->opened)
    e->transport.close(e->transport.opaque);
  e->opened = 0;
}
void wa_source_init(struct wa_source *s, const struct wa_format *f) {
  w32_fill(s, 0, sizeof(*s));
  s->format = *f;
  s->frequency = f->rate;
  s->epoch = 1;
  wa_source_gain(s);
}
uint32_t wa_register(struct wa_engine *e, struct wa_source *s) {
  for (unsigned i = 0; i < WA_SOURCES; i++)
    if (!e->sources[i]) {
      s->slot = i;
      s->epoch = ++e->slot_epoch[i];
      s->active = 1;
      e->sources[i] = s;
      e->source_count++;
      return 0;
    }
  return WA_ALLOCATED;
}
void wa_cancel_notifications(struct wa_engine *e, struct wa_source *s) {
  unsigned kept = 0;
  for (unsigned i = 0; i < e->callback_count; i++) {
    struct wa_event v = e->events[(e->callback_head + i) % WA_CALLBACKS];
    if (v.message == 0 && v.handle == s->handle && !v.header)
      continue;
    e->events[(e->callback_head + kept++) % WA_CALLBACKS] = v;
  }
  e->callback_count = kept;
}
void wa_unregister(struct wa_engine *e, struct wa_source *s) {
  if (s->active && e->sources[s->slot] == s) {
    wa_cancel_notifications(e, s);
    e->slot_epoch[s->slot] = s->epoch;
    e->sources[s->slot] = 0;
    e->source_count--;
    s->active = 0;
    s->epoch++;
  }
}
uint32_t wa_db_gain(int32_t db) {
  if (db <= -10000)
    return 0;
  if (db >= 0)
    return 32768; /* one centibel table, 0.01 dB steps */
  return wa_gain_table[-db];
}
void wa_source_gain(struct wa_source *s) {
  uint32_t g = wa_db_gain(s->volume);
  s->gain[0] = g;
  s->gain[1] = g;
  if (s->pan > 0)
    s->gain[0] = g * wa_db_gain(-s->pan) / 32768;
  else if (s->pan < 0)
    s->gain[1] = g * wa_db_gain(s->pan) / 32768;
}
uint32_t wa_event(struct wa_engine *e, const struct wa_event *v) {
  if (e->callback_count == WA_CALLBACKS)
    return WA_ALLOCATED;
  e->events[(e->callback_head + e->callback_count++) % WA_CALLBACKS] = *v;
  return 0;
}
/* Stable insertion order: rendering frame, source registration slot, offset
 * declaration order; stop follows crossed offsets, wave DONE follows last PCM.
 */
static void notify(struct wa_engine *e, struct wa_source *s, uint32_t from,
                   uint32_t advance, int stop, uint64_t due) {
  uint32_t bytes = s->samples ? s->samples->bytes : 0;
  /* Temporal crossing order, declaration order for identical offsets. */
  for (uint32_t step = 1; bytes && step <= advance; step++) {
    uint32_t at = (from + step) % bytes;
    for (unsigned k = 0; k < s->notifications; k++)
      if (s->notify[k].offset == at) {
        struct wa_event v = {.due = due,
                             .handle = s->handle,
                             .callback = {.target = s->notify[k].event,
                                          .kind = WA_CALLBACK_EVENT}};
        wa_event(e, &v);
      }
  }
  if (stop)
    for (unsigned k = 0; k < s->notifications; k++)
      if (s->notify[k].offset == WA_STOP) {
        struct wa_event v = {.due = due,
                             .handle = s->handle,
                             .callback = {.target = s->notify[k].event,
                                          .kind = WA_CALLBACK_EVENT}};
        wa_event(e, &v);
      }
}
void wa_stop(struct wa_engine *e, struct wa_source *s) {
  if (s->playing || s->end_frame > e->consumed) {
    wa_cancel_notifications(e, s);
    s->playing = 0;
    s->end_frame = e->consumed;
    s->frame = s->heard;
    s->fraction = s->heard_fraction;
    s->epoch++;
    notify(e, s, 0, 0, 1, e->consumed);
  }
}
void wa_position(struct wa_source *s, uint32_t byte) {
  s->frame = s->heard = byte / s->format.align;
  s->fraction = s->heard_fraction = 0;
  s->epoch++;
}
uint32_t wa_write_cursor(const struct wa_engine *e, const struct wa_source *s) {
  if (!s->samples || !s->samples->bytes)
    return 0;
  uint32_t frames = s->samples->bytes / s->format.align;
  uint32_t ahead = (WA_LEAD * s->frequency + e->rate - 1) / e->rate;
  return ((s->heard + ahead) % frames) * s->format.align;
}
uint32_t wa_write(struct wa_engine *e, struct wa_source *s,
                  struct wa_header *h) {
  if (!h || !(h->flags & WA_PREPARED))
    return 34;
  if (h->flags & WA_QUEUED)
    return WA_BUSY;
  if (!h->data || !h->bytes || h->bytes % s->format.align)
    return WA_INVALID;
  if (s->header_at && !s->loop_count) {
    uint32_t used = s->header_at;
    for (unsigned i = used; i < s->header_count; i++)
      s->headers[i - used] = s->headers[i];
    s->header_count -= used;
    s->header_at = 0;
  }
  if (e->header_count == WA_HEADERS || s->header_count == WA_HEADERS)
    return WA_ALLOCATED;
  if (!s->header_count) {
    s->loop_start = s->loop_count = s->break_loop = 0;
  }
  h->flags = (h->flags & ~WA_DONE) | WA_QUEUED;
  s->headers[s->header_count++] = h;
  e->header_count++;
  s->playing = 1;
  return 0;
}
static void done(struct wa_engine *e, struct wa_source *s, struct wa_header *h,
                 uint64_t due) {
  struct wa_event v = {.due = due,
                       .callback = s->callback,
                       .handle = s->handle,
                       .p1 = (uintptr_t)h,
                       .message = 0x3bd,
                       .header = h};
  wa_event(e, &v);
}
uint32_t wa_reset(struct wa_engine *e, struct wa_source *s) {
  for (unsigned k = 0; k < e->callback_count; k++) {
    struct wa_event *v = &e->events[(e->callback_head + k) % WA_CALLBACKS];
    if (v->header && v->handle == s->handle)
      v->due = e->consumed;
  }
  uint32_t n = s->header_count - s->header_at;
  if (n > WA_CALLBACKS - e->callback_count)
    return WA_ALLOCATED;
  for (unsigned i = s->header_at; i < s->header_count; i++)
    done(e, s, s->headers[i], e->consumed);
  s->header_at = s->header_count = s->header_frame = 0;
  s->rendered_samples = s->heard_samples = 0;
  s->loop_start = s->loop_count = s->break_loop = 0;
  s->playing = s->paused = 0;
  for (unsigned k = 0; k < e->callback_count; k++) {
    struct wa_event *v = &e->events[(e->callback_head + k) % WA_CALLBACKS];
    if (v->header && v->handle == s->handle) {
      v->header->flags = (v->header->flags & ~WA_QUEUED) | WA_DONE;
      v->header = 0;
      if (e->header_count)
        e->header_count--;
    }
  }
  wa_position(s, 0);
  return 0;
}
static int32_t decode(const uint8_t *p, uint32_t bits) {
  return bits == 8 ? ((int32_t)*p - 128) * 256 : (int16_t)w32_u16(p);
}
static int32_t lerp(int32_t a, int32_t b, uint32_t phase, uint32_t rate) {
  int32_t d = b - a;
  uint32_t m = (uint32_t)(d < 0 ? -d : d);
  int32_t v = (int32_t)(m * phase / rate);
  return a + (d < 0 ? -v : v);
}
static void read_frame(struct wa_source *s, const uint8_t *p, int32_t out[2]) {
  out[0] = decode(p, s->format.bits);
  out[1] = s->format.channels == 1
               ? out[0]
               : decode(p + s->format.bits / 8, s->format.bits);
}
static void frame(struct wa_engine *e, struct wa_source *s, int32_t out[2],
                  uint64_t due) {
  uint32_t total;
  const uint8_t *p;
  struct wa_header *h = 0;
  if (s->wave) {
    if (s->header_at == s->header_count) {
      s->playing = 0;
      return;
    }
    h = s->headers[s->header_at];
    total = h->bytes / s->format.align;
    p = (const uint8_t *)h->data;
    s->frame = s->header_frame;
    if ((h->flags & WA_BEGINLOOP) && !s->header_frame && !s->loop_count) {
      s->loop_start = s->header_at;
      s->loop_count = h->loops ? h->loops : 1;
    }
  } else {
    if (!s->samples)
      return;
    total = s->samples->bytes / s->format.align;
    p = s->samples->data;
  }
  if (!total || s->frame >= total)
    return;
  uint32_t next = s->frame + 1;
  const uint8_t *q = p;
  uint32_t qframe = next;
  if (next == total) {
    qframe = s->looping ? 0 : total - 1;
    if (s->wave && s->header_at + 1 < s->header_count) {
      q = (const uint8_t *)s->headers[s->header_at + 1]->data;
      qframe = 0;
    }
  }
  int32_t a[2], b[2];
  read_frame(s, p + s->frame * s->format.align, a);
  read_frame(s, q + qframe * s->format.align, b);
  int mute = !e->focused && !(s->flags & WA_GLOBAL);
  for (unsigned c = 0; c < 2; c++)
    out[c] = mute ? 0
                  : lerp(a[c], b[c], s->fraction, e->rate) *
                        (int32_t)s->gain[c] / 32768;
  uint32_t from = s->frame * s->format.align;
  s->fraction += s->frequency;
  uint32_t advance = s->fraction / e->rate;
  s->fraction %= e->rate;
  s->frame += advance;
  s->rendered_samples += advance;
  if (s->wave) {
    while (s->frame >= total) {
      s->frame -= total;
      if ((h->flags & WA_ENDLOOP) && s->loop_count > 1 && !s->break_loop) {
        s->loop_count--;
        s->header_at = s->loop_start;
      } else {
        if (h->flags & WA_ENDLOOP) {
          s->loop_count = 0;
          for (unsigned i = s->loop_start; i < s->header_at; i++)
            done(e, s, s->headers[i], due);
        }
        if (!s->loop_count)
          done(e, s, h, due);
        s->header_at++;
      }
      if (s->header_at == s->header_count) {
        s->playing = 0;
        s->frame = 0;
        break;
      }
      h = s->headers[s->header_at];
      total = h->bytes / s->format.align;
    }
    s->header_frame = s->frame;
  } else {
    int stop = !s->looping && s->frame >= total;
    notify(e, s, from, advance * s->format.align, stop, due);
    if (s->frame >= total) {
      s->frame = s->looping ? s->frame % total : 0;
      if (stop) {
        s->playing = 0;
        s->end_frame = due;
      }
    }
  }
}
uint32_t wa_render(struct wa_engine *e, int16_t *out, uint32_t n) {
  n = min(n, WA_QUANTUM);
  uint32_t i;
  for (i = 0; i < n; i++) { /* Worst case reserve covers every source's offsets
                               and wave heads. */
    uint32_t reserve = 0;
    for (unsigned k = 0; k < WA_SOURCES; k++) {
      struct wa_source *s = e->sources[k];
      if (s && s->playing && !s->paused) {
        uint32_t repeats = 1;
        if (s->samples) {
          uint32_t frames = s->samples->bytes / s->format.align;
          repeats += (s->frequency + e->rate - 1) / e->rate / frames;
        }
        reserve += s->notifications * repeats +
                   (s->wave ? s->header_count - s->header_at : 0);
      }
    }
    if (reserve > WA_CALLBACKS - e->callback_count)
      break;
    int32_t sum[2] = {0, 0};
    uint64_t due = e->accepted + e->pending_frames + i + 1;
    for (unsigned k = 0; k < WA_SOURCES; k++) {
      struct wa_source *s = e->sources[k];
      if (!s)
        continue;
      if (s->playing && !s->paused) {
        int32_t v[2] = {0, 0};
        frame(e, s, v, due);
        sum[0] += v[0];
        sum[1] += v[1];
      }
      e->ledger[(uint32_t)(due - 1) % WA_LEAD][k] = (struct wa_stamp){
          s->frame, s->fraction, s->epoch, s->rendered_samples};
    }
    if (e->synth)
      e->synth(e->synth_context, sum, 1, e->rate);
    e->rendered++;
    for (unsigned c = 0; c < 2; c++) {
      int32_t v = sum[c];
      uint32_t magnitude = (uint32_t)(v < 0 ? -v : v);
      uint32_t gain = e->volume[c];
      magnitude =
          (magnitude / 65535) * gain + (magnitude % 65535) * gain / 65535;
      v = v < 0 ? -(int32_t)magnitude : (int32_t)magnitude;
      if (v > 32767)
        v = 32767;
      else if (v < -32768)
        v = -32768;
      out[i * 2 + c] = (int16_t)v;
    }
  }
  return i;
}
void wa_consumed(struct wa_engine *e, uint64_t n) {
  if (n > e->accepted)
    n = e->accepted;
  if (n < e->consumed)
    return;
  while (e->consumed < n) {
    unsigned at = (unsigned)e->consumed % WA_LEAD;
    for (unsigned k = 0; k < WA_SOURCES; k++) {
      struct wa_source *s = e->sources[k];
      struct wa_stamp *v = &e->ledger[at][k];
      if (s && s->epoch == v->epoch) {
        s->heard = v->frame;
        s->heard_fraction = v->fraction;
        s->heard_samples = v->samples;
      }
    }
    e->consumed++;
  }
}
void wa_dispatch(
    struct wa_engine *e) { /* A later immediate reset callback can precede an
                              unconsumed notification. */
  for (unsigned n = 0; n < 64; n++) {
    unsigned found = e->callback_count;
    for (unsigned k = 0; k < e->callback_count; k++)
      if (e->events[(e->callback_head + k) % WA_CALLBACKS].due <= e->consumed) {
        found = k;
        break;
      }
    if (found == e->callback_count)
      break;
    struct wa_event v = e->events[(e->callback_head + found) % WA_CALLBACKS];
    for (unsigned k = found; k + 1 < e->callback_count; k++)
      e->events[(e->callback_head + k) % WA_CALLBACKS] =
          e->events[(e->callback_head + k + 1) % WA_CALLBACKS];
    e->callback_count--;
    if (v.completion_flags)
      *v.completion_flags =
          (*v.completion_flags & ~v.completion_clear) | v.completion_set;
    if (v.completion_count && *v.completion_count)
      (*v.completion_count)--;
    if (v.header) {
      v.header->flags = (v.header->flags & ~WA_QUEUED) | WA_DONE;
      if (e->header_count)
        e->header_count--;
    }
    if (e->transport.dispatch)
      e->transport.dispatch(e->transport.opaque, &v);
  }
}
uint32_t wa_poll(struct wa_engine *e) {
  if (!e->opened)
    return 0;
  uint64_t consumed;
  uint32_t generation;
  if (e->transport.status(e->transport.opaque, &consumed, &generation) < 0 ||
      generation != e->generation || consumed < e->base_consumed)
    return 6;
  wa_consumed(e, consumed - e->base_consumed);
  return 0;
}
uint32_t wa_pump(struct wa_engine *e) {
  if (!e->opened)
    return 0;
  uint32_t error = wa_poll(e);
  if (error)
    return error;
  uint32_t lead = (uint32_t)(e->accepted - e->consumed);
  if (lead >= WA_LEAD)
    return 0;
  if (!e->pending_frames) {
    uint32_t count = min(WA_QUANTUM, WA_LEAD - lead);
    e->pending_at = 0;
    e->pending_frames = wa_render(e, e->pending, count);
  }
  if (e->pending_frames) {
    uint32_t count = min(e->pending_frames, WA_LEAD - lead);
    int r = e->transport.write(e->transport.opaque,
                               e->pending + e->pending_at * 2, count);
    if (r < 0)
      return r == -11 ? 0 : 6;
    if ((uint32_t)r > count)
      return 6;
    e->accepted += (uint32_t)r;
    e->pending_at += (uint32_t)r;
    e->pending_frames -= (uint32_t)r;
  }
  return 0;
}
