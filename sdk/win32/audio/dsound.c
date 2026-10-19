/* Original DirectSound software device. SPDX-License-Identifier:
 * LGPL-2.1-or-later */
#include "audio.h"
#define API W32API
#define DSERR_INVALIDPARAM ((int32_t)0x80070057)
#define DSERR_INVALIDCALL ((int32_t)0x88780032)
#define DSERR_ALLOCATED ((int32_t)0x8878000a)
#define DSERR_CONTROLUNAVAIL ((int32_t)0x8878001e)
#define DSERR_NODRIVER ((int32_t)0x88780078)
#define E_NOINTERFACE ((int32_t)0x80004002)
#define E_OUTOFMEMORY ((int32_t)0x8007000e)
struct guid {
  uint32_t a;
  uint16_t b, c;
  uint8_t d[8];
};
struct com {
  const void *const *vt;
  void *owner;
  uint32_t kind;
};
struct vector {
  float x, y, z;
};
struct spatial {
  uint32_t size;
  struct vector position, velocity;
  uint32_t inside, outside;
  struct vector cone;
  int32_t cone_volume;
  float minimum, maximum;
  uint32_t mode;
};
struct listener {
  uint32_t size;
  struct vector position, velocity, front, top;
  float distance, rolloff, doppler;
};
struct device {
  struct com com, listener;
  uint32_t refs, coop, speakers;
  struct wa_engine *engine;
  struct listener live, deferred;
  uint32_t dirty;
};
struct buffer {
  struct com com, notify, spatial, ks;
  uint32_t refs;
  struct device *device;
  struct wa_source source;
  struct spatial live, deferred;
  uint32_t dirty, locked;
  void *lock1, *lock2;
  uint32_t bytes1, bytes2;
};
struct desc {
  uint32_t size, flags, bytes, reserved;
  struct wa_format *format;
  struct guid algorithm;
};
static const void *const device_vt[];
static const void *const buffer_vt[];
static const void *const notify_vt[];
static const void *const spatial_vt[];
static const void *const listener_vt[];
static const void *const ks_vt[];

static struct buffer *buffers[WA_SOURCES];
static const struct guid iid_unknown = {
    0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
static const struct guid iid_ds = {
    0x279afa83, 0x4981, 0x11ce, {0xa5, 0x21, 0, 0x20, 0xaf, 0x0b, 0xe5, 0x60}};
static const struct guid iid_ds8 = {
    0xc50a7e93, 0xf395, 0x4834, {0x9e, 0xf6, 0x7f, 0xa9, 0x9d, 0xe5, 9, 0x66}};
static const struct guid iid_buf = {
    0x279afa85, 0x4981, 0x11ce, {0xa5, 0x21, 0, 0x20, 0xaf, 0x0b, 0xe5, 0x60}};
static const struct guid iid_buf8 = {
    0x6825a449,
    0x7524,
    0x4d82,
    {0x92, 0x0f, 0x50, 0xe3, 0x6a, 0xb3, 0xab, 0x1e}};
static const struct guid iid_notify = {
    0xb0210783, 0x89cd, 0x11d0, {0xaf, 8, 0, 0xa0, 0xc9, 0x25, 0xcd, 0x16}};
static const struct guid iid_spatial = {
    0x279afa86, 0x4981, 0x11ce, {0xa5, 0x21, 0, 0x20, 0xaf, 0x0b, 0xe5, 0x60}};
static const struct guid iid_listener = {
    0x279afa84, 0x4981, 0x11ce, {0xa5, 0x21, 0, 0x20, 0xaf, 0x0b, 0xe5, 0x60}};
static const struct guid iid_ks = {
    0x31efac30, 0x515c, 0x11d0, {0xa9, 0xaa, 0, 0xaa, 0, 0x61, 0xbe, 0x93}};
static int equal(const struct guid *a, const struct guid *b) {
  if (!a)
    return 0;
  const uint8_t *p = (const void *)a, *q = (const void *)b;
  for (unsigned i = 0; i < 16; i++)
    if (p[i] != q[i])
      return 0;
  return 1;
}
static struct buffer *buf(struct com *c) { return c ? c->owner : 0; }
static struct device *dev(struct com *c) { return c ? c->owner : 0; }
static uint32_t API add(struct com *c) {
  CiukiAudioLock();
  uint32_t r = c->kind == 1 || c->kind == 2 ? ++dev(c)->refs : ++buf(c)->refs;
  CiukiAudioUnlock();
  return r;
}
static uint32_t API release(struct com *c) {
  CiukiAudioLock();
  uint32_t r;
  if (c->kind == 1 || c->kind == 2) {
    struct device *d = dev(c);
    r = --d->refs;
    if (!r)
      wa_free(d);
  } else {
    struct buffer *b = buf(c);
    r = --b->refs;
    if (!r) {
      wa_unregister(b->device->engine, &b->source);
      for (unsigned i = 0; i < WA_SOURCES; i++)
        if (buffers[i] == b)
          buffers[i] = 0;
      if (b->source.samples && !--b->source.samples->refs)
        wa_free(b->source.samples);
      release(&b->device->com);
      wa_free(b);
    }
  }
  CiukiAudioUnlock();
  return r;
}
static int32_t API query(struct com *c, const struct guid *id, void **out) {
  if (!out)
    return DSERR_INVALIDPARAM;
  *out = 0;
  if (!c || !id)
    return DSERR_INVALIDPARAM;
  if (c->kind <= 2) {
    struct device *d = dev(c);
    if (equal(id, &iid_unknown) || equal(id, &iid_ds) || equal(id, &iid_ds8))
      *out = &d->com;
    else if (equal(id, &iid_listener))
      *out = &d->listener;
  } else {
    struct buffer *b = buf(c);
    if (equal(id, &iid_unknown) || equal(id, &iid_buf) ||
        (!(b->source.flags & 1) && equal(id, &iid_buf8)))
      *out = &b->com;
    else if (equal(id, &iid_notify) && (b->source.flags & 0x100))
      *out = &b->notify;
    else if (equal(id, &iid_spatial) && (b->source.flags & 0x10))
      *out = &b->spatial;
    else if (equal(id, &iid_listener) && (b->source.flags & 1))
      *out = &b->device->listener;
    else if (equal(id, &iid_ks))
      *out = &b->ks;
  }
  if (!*out)
    return E_NOINTERFACE;
  add(*out);
  return 0;
}
static float square_root(float x) {
  if (x <= 0)
    return 0;
  union {
    float f;
    uint32_t u;
  } v = {.f = x};
  v.u = (v.u >> 1) + 0x1fc00000u;
  float r = v.f;
  for (unsigned i = 0; i < 6; i++)
    r = (r + x / r) * 0.5f;
  return r;
}
static void spatial_gain(struct buffer *b) {
  struct wa_source *s = &b->source;
  wa_source_gain(s);
  if (!(s->flags & 0x10) || b->live.mode == 2)
    return;
  struct listener *l = &b->device->live;
  float x = b->live.position.x, y = b->live.position.y, z = b->live.position.z;
  if (!b->live.mode) {
    x -= l->position.x;
    y -= l->position.y;
    z -= l->position.z;
  }
  float d2 = x * x + y * y + z * z;
  float distance = square_root(d2);
  float d = distance;
  if (d > b->live.maximum)
    d = b->live.maximum;
  float gain = 1;
  if (d > b->live.minimum)
    gain = b->live.minimum /
           (b->live.minimum + l->rolloff * (d - b->live.minimum));
  float right_x = 1, right_y = 0, right_z = 0;
  if (!b->live.mode) {
    right_x = l->top.y * l->front.z - l->top.z * l->front.y;
    right_y = l->top.z * l->front.x - l->top.x * l->front.z;
    right_z = l->top.x * l->front.y - l->top.y * l->front.x;
  }
  float length =
      square_root(right_x * right_x + right_y * right_y + right_z * right_z);
  float pan = distance && length ? (x * right_x + y * right_y + z * right_z) /
                                       (distance * length)
                                 : 0;
  if (pan > 1)
    pan = 1;
  if (pan < -1)
    pan = -1;
  for (unsigned c = 0; c < 2; c++) {
    float g = gain * (c ? (pan < 0 ? 1 + pan : 1) : (pan > 0 ? 1 - pan : 1));
    s->gain[c] = (uint32_t)(s->gain[c] * g);
  }
}
static int32_t allocate_buffer(struct com *c, const struct desc *a,
                               struct com **out, void *outer,
                               struct wa_samples *shared) {
  if (!out)
    return DSERR_INVALIDPARAM;
  *out = 0;
  if (!a || a->size < 20 || outer)
    return DSERR_INVALIDPARAM;
  struct device *d = dev(c);
  struct wa_format f = {1, 2, 44100, 176400, 4, 16, 0};
  uint32_t bytes = a->bytes;
  if (a->flags & 1) {
    if (bytes || a->format)
      return DSERR_INVALIDPARAM;
    bytes = 8192;
  } else {
    if (!wa_valid_format(a->format) || bytes < 4 ||
        bytes > a->format->average * 30 || bytes % a->format->align)
      return DSERR_INVALIDPARAM;
    f = *a->format;
  }
  if ((a->flags & 0x10) && f.channels != 1)
    return DSERR_INVALIDPARAM;
  CiukiAudioLock();
  unsigned slot;
  for (slot = 0; slot < WA_SOURCES && buffers[slot]; slot++) {
  }
  if (slot == WA_SOURCES) {
    CiukiAudioUnlock();
    return DSERR_ALLOCATED;
  }
  struct buffer *b = wa_alloc(sizeof(*b));
  struct wa_samples *m = shared ? shared : wa_alloc(sizeof(*m) + bytes);
  if (!b || !m) {
    wa_free(b);
    if (!shared)
      wa_free(m);
    CiukiAudioUnlock();
    return E_OUTOFMEMORY;
  }
  if (!shared) {
    m->refs = 1;
    m->bytes = bytes;
    w32_fill(m->data, f.bits == 8 ? 128 : 0, bytes);
  }
  b->com = (struct com){buffer_vt, b, 3};
  b->notify = (struct com){notify_vt, b, 4};
  b->spatial = (struct com){spatial_vt, b, 5};
  b->ks = (struct com){ks_vt, b, 6};
  b->refs = 1;
  b->device = d;
  wa_source_init(&b->source, &f);
  b->source.samples = m;
  b->source.flags = a->flags | 0x8;
  b->source.handle = (uintptr_t)b;
  b->live.size = sizeof(b->live);
  b->live.minimum = 1;
  b->live.inside = b->live.outside = 360;
  b->live.cone.z = 1;
  b->live.maximum = 1e9f;
  b->deferred = b->live;
  uint32_t r = wa_register(d->engine, &b->source);
  if (r) {
    if (!shared)
      wa_free(m);
    wa_free(b);
    CiukiAudioUnlock();
    return DSERR_ALLOCATED;
  }
  if (shared)
    m->refs++;
  buffers[slot] = b;
  add(c);
  *out = &b->com;
  CiukiAudioUnlock();
  return 0;
}
static int32_t API create_buffer(struct com *c, const struct desc *a,
                                 struct com **out, void *outer) {
  return allocate_buffer(c, a, out, outer, 0);
}
static int32_t API duplicate(struct com *c, struct com *original,
                             struct com **out) {
  if (!out || !original || original->vt != buffer_vt)
    return DSERR_INVALIDPARAM;
  struct buffer *a = buf(original);
  if (a->source.flags & 1)
    return DSERR_INVALIDCALL;
  struct desc desc = {sizeof(desc),
                      a->source.flags,
                      a->source.samples->bytes,
                      0,
                      &a->source.format,
                      {0}};
  int32_t r = allocate_buffer(c, &desc, out, 0, a->source.samples);
  if (r)
    return r;
  CiukiAudioLock();
  struct buffer *b = buf(*out);
  b->source.volume = a->source.volume;
  b->source.pan = a->source.pan;
  b->source.frequency = a->source.frequency;
  b->live = a->live;
  spatial_gain(b);
  CiukiAudioUnlock();
  return 0;
}
static int32_t API cooperative(struct com *c, void *hwnd, uint32_t level) {
  (void)hwnd;
  if (level < 1 || level > 4)
    return DSERR_INVALIDPARAM;
  dev(c)->coop = level;
  return 0;
}
static int32_t API ds_caps(struct com *c, uint32_t *p) {
  (void)c;
  if (!p || p[0] < 96)
    return DSERR_INVALIDPARAM;
  uint32_t size = p[0];
  w32_fill(p, 0, 96);
  p[0] = size;
  p[1] = 0xf1f; /* primary/secondary mono/stereo 8/16-bit, continuous rates */
  p[2] = 4000;
  p[3] = 48000;
  p[4] = 1;
  return 0;
}
static int32_t API compact(struct com *c) {
  (void)c;
  return 0;
}
static int32_t API get_speaker(struct com *c, uint32_t *p) {
  if (!p)
    return DSERR_INVALIDPARAM;
  *p = dev(c)->speakers;
  return 0;
}
static int32_t API set_speaker(struct com *c, uint32_t n) {
  dev(c)->speakers = n;
  return 0;
}
static int32_t API ds_initialize(struct com *c, const struct guid *g) {
  (void)c;
  (void)g;
  return 0;
}
static int32_t API certify(struct com *c, uint32_t *p) {
  (void)c;
  if (!p)
    return DSERR_INVALIDPARAM;
  *p = 1;
  return 0;
}
static int32_t API buffer_caps(struct com *c, uint32_t *p) {
  struct buffer *b = buf(c);
  if (!p || p[0] < 20)
    return DSERR_INVALIDPARAM;
  p[1] = b->source.flags;
  p[2] = b->source.samples->bytes;
  p[3] = p[4] = 0;
  return 0;
}
static int32_t API position(struct com *c, uint32_t *play, uint32_t *write) {
  CiukiAudioLock();
  struct buffer *b = buf(c);
  if (wa_poll(b->device->engine)) {
    CiukiAudioUnlock();
    return DSERR_NODRIVER;
  }
  if (play)
    *play = b->source.heard * b->source.format.align;
  if (write)
    *write = wa_write_cursor(b->device->engine, &b->source);
  CiukiAudioUnlock();
  return 0;
}
static int32_t API get_format(struct com *c, void *out, uint32_t n,
                              uint32_t *written) {
  if (written)
    *written = 18;
  if (!out)
    return written ? 0 : DSERR_INVALIDPARAM;
  if (n < 18)
    return DSERR_INVALIDPARAM;
  w32_copy(out, &buf(c)->source.format, 18);
  return 0;
}
static int32_t API get_volume(struct com *c, int32_t *p) {
  if (!p)
    return DSERR_INVALIDPARAM;
  *p = buf(c)->source.volume;
  return 0;
}
static int32_t API get_pan(struct com *c, int32_t *p) {
  if (!p)
    return DSERR_INVALIDPARAM;
  *p = buf(c)->source.pan;
  return 0;
}
static int32_t API get_frequency(struct com *c, uint32_t *p) {
  if (!p)
    return DSERR_INVALIDPARAM;
  *p = buf(c)->source.frequency;
  return 0;
}
static int32_t API status(struct com *c, uint32_t *p) {
  if (!p)
    return DSERR_INVALIDPARAM;
  struct wa_source *s = &buf(c)->source;
  uint32_t running =
      s->playing || s->end_frame > buf(c)->device->engine->consumed;
  *p = (running ? 1u : 0u) | (running && s->looping ? 4u : 0u);
  return 0;
}
static int32_t API buffer_initialize(struct com *c, void *d,
                                     const struct desc *a) {
  (void)c;
  (void)d;
  (void)a;
  return (int32_t)0x88780082;
}
static int32_t API lock_buffer(struct com *c, uint32_t offset, uint32_t n,
                               void **p1, uint32_t *n1, void **p2, uint32_t *n2,
                               uint32_t flags) {
  struct buffer *b = buf(c);
  if ((b->source.flags & 1) && b->device->coop != 4)
    return DSERR_INVALIDCALL;
  if (!p1 || !n1 || (!!p2 != !!n2) || flags & ~3u)
    return DSERR_INVALIDPARAM;
  CiukiAudioLock();
  uint32_t bytes = b->source.samples->bytes;
  if (flags & 1)
    offset = wa_write_cursor(b->device->engine, &b->source);
  if (flags & 2)
    n = bytes;
  if (offset >= bytes || !n || n > bytes || b->locked) {
    CiukiAudioUnlock();
    return DSERR_INVALIDPARAM;
  }
  uint32_t first = n < bytes - offset ? n : bytes - offset;
  *p1 = b->source.samples->data + offset;
  *n1 = first;
  b->lock1 = *p1;
  b->bytes1 = first;
  b->lock2 = 0;
  b->bytes2 = 0;
  if (p2) {
    *p2 = n > first ? b->source.samples->data : 0;
    *n2 = n - first;
    b->lock2 = *p2;
    b->bytes2 = *n2;
  }
  b->locked = 1;
  CiukiAudioUnlock();
  return 0;
}
static int32_t API unlock_buffer(struct com *c, void *p1, uint32_t n1, void *p2,
                                 uint32_t n2) {
  struct buffer *b = buf(c);
  CiukiAudioLock();
  int valid = b->locked && p1 == b->lock1 && p2 == b->lock2 &&
              n1 <= b->bytes1 && n2 <= b->bytes2;
  if (valid)
    b->locked = 0;
  CiukiAudioUnlock();
  return valid ? 0 : DSERR_INVALIDPARAM;
}
static int32_t API play(struct com *c, uint32_t reserved, uint32_t priority,
                        uint32_t flags) {
  (void)priority;
  if (reserved || flags & ~0x1fu)
    return DSERR_INVALIDPARAM;
  struct buffer *b = buf(c);
  if ((b->source.flags & 1) && b->device->coop != 4)
    return DSERR_INVALIDCALL;
  CiukiAudioLock();
  b->source.playing = 1;
  b->source.looping = !!(flags & 1);
  CiukiAudioUnlock();
  return 0;
}
static int32_t API set_position(struct com *c, uint32_t n) {
  struct buffer *b = buf(c);
  if (n >= b->source.samples->bytes || n % b->source.format.align)
    return DSERR_INVALIDPARAM;
  CiukiAudioLock();
  wa_cancel_notifications(b->device->engine, &b->source);
  wa_position(&b->source, n);
  CiukiAudioUnlock();
  return 0;
}
static int32_t API set_format(struct com *c, const struct wa_format *f) {
  struct buffer *b = buf(c);
  if (!(b->source.flags & 1))
    return DSERR_INVALIDCALL;
  if (!wa_valid_format(f))
    return DSERR_INVALIDPARAM;
  CiukiAudioLock();
  b->source.format = *f;
  b->source.frequency = f->rate;
  wa_position(&b->source, 0);
  CiukiAudioUnlock();
  return 0;
}
static int32_t API set_volume(struct com *c, int32_t v) {
  struct buffer *b = buf(c);
  if (!(b->source.flags & 0x80))
    return DSERR_CONTROLUNAVAIL;
  if (v > 0 || v < -10000)
    return DSERR_INVALIDPARAM;
  CiukiAudioLock();
  b->source.volume = v;
  spatial_gain(b);
  CiukiAudioUnlock();
  return 0;
}
static int32_t API set_pan(struct com *c, int32_t v) {
  struct buffer *b = buf(c);
  if (!(b->source.flags & 0x40) || (b->source.flags & 0x10))
    return DSERR_CONTROLUNAVAIL;
  if (v > 10000 || v < -10000)
    return DSERR_INVALIDPARAM;
  CiukiAudioLock();
  b->source.pan = v;
  spatial_gain(b);
  CiukiAudioUnlock();
  return 0;
}
static int32_t API set_frequency(struct com *c, uint32_t v) {
  struct buffer *b = buf(c);
  if (!(b->source.flags & 0x20))
    return DSERR_CONTROLUNAVAIL;
  if (v && (v < 100 || v > 100000))
    return DSERR_INVALIDPARAM;
  CiukiAudioLock();
  b->source.frequency = v ? v : b->source.format.rate;
  CiukiAudioUnlock();
  return 0;
}
static int32_t API stop(struct com *c) {
  CiukiAudioLock();
  struct buffer *b = buf(c);
  wa_stop(b->device->engine, &b->source);
  CiukiAudioUnlock();
  return 0;
}
static int32_t API restore(struct com *c) {
  (void)c;
  return 0;
}
static int32_t API set_fx(struct com *c, uint32_t n, void *fx,
                          uint32_t *results) {
  (void)c;
  (void)fx;
  for (unsigned i = 0; results && i < n; i++)
    results[i] = 4;
  return DSERR_CONTROLUNAVAIL;
}
static int32_t API acquire_resources(struct com *c, uint32_t flags, uint32_t n,
                                     uint32_t *r) {
  (void)c;
  (void)flags;
  for (unsigned i = 0; r && i < n; i++)
    r[i] = 0;
  return 0;
}
static int32_t API object_path(struct com *c, const struct guid *g, uint32_t i,
                               const struct guid *id, void **out) {
  (void)c;
  (void)g;
  (void)i;
  (void)id;
  if (out)
    *out = 0;
  return E_NOINTERFACE;
}
static int32_t API notifications(struct com *c, uint32_t n,
                                 const struct wa_notify *v) {
  struct buffer *b = buf(c);
  if (n > WA_NOTIFICATIONS || (n && !v))
    return DSERR_INVALIDPARAM;
  CiukiAudioLock();
  uint32_t total = n;
  for (unsigned i = 0; i < WA_SOURCES; i++)
    if (buffers[i] && buffers[i] != b)
      total += buffers[i]->source.notifications;
  if (total > WA_NOTIFICATIONS || b->source.playing) {
    CiukiAudioUnlock();
    return DSERR_INVALIDCALL;
  }
  for (unsigned i = 0; i < n; i++)
    if (!v[i].event ||
        (v[i].offset != WA_STOP && v[i].offset >= b->source.samples->bytes)) {
      CiukiAudioUnlock();
      return DSERR_INVALIDPARAM;
    }
  w32_copy(b->source.notify, v, n * sizeof(*v));
  b->source.notifications = n;
  CiukiAudioUnlock();
  return 0;
}
static int32_t API ks_set(struct com *c, const struct guid *g, uint32_t id,
                          void *inst, uint32_t ni, void *data, uint32_t nd) {
  (void)c;
  (void)g;
  (void)id;
  (void)inst;
  (void)ni;
  (void)data;
  (void)nd;
  return (int32_t)0x80070490;
}
static int32_t API ks_get(struct com *c, const struct guid *g, uint32_t id,
                          void *inst, uint32_t ni, void *data, uint32_t nd,
                          uint32_t *got) {
  if (got)
    *got = 0;
  return ks_set(c, g, id, inst, ni, data, nd);
}
static int32_t API ks_support(struct com *c, const struct guid *g, uint32_t id,
                              uint32_t *support) {
  (void)c;
  (void)g;
  (void)id;
  if (support)
    *support = 0;
  return (int32_t)0x80070490;
}
/* 3D compatibility is distance/pan only; orientation/cones/velocity are
 * retained for round-trips. Deferred state is committed atomically. */
static int finite(float x) { return x == x && x >= -1e15f && x <= 1e15f; }
static int valid_vec(struct vector v) {
  return finite(v.x) && finite(v.y) && finite(v.z);
}
static int32_t API s_get_all(struct com *c, struct spatial *p) {
  if (!p || p->size != sizeof(*p))
    return DSERR_INVALIDPARAM;
  *p = buf(c)->live;
  return 0;
}
static int32_t API s_get_cones(struct com *c, uint32_t *a, uint32_t *b) {
  if (!a || !b)
    return DSERR_INVALIDPARAM;
  *a = buf(c)->live.inside;
  *b = buf(c)->live.outside;
  return 0;
}
static int32_t API s_get_cone(struct com *c, struct vector *p) {
  if (!p)
    return DSERR_INVALIDPARAM;
  *p = buf(c)->live.cone;
  return 0;
}
static int32_t API s_get_conevol(struct com *c, int32_t *p) {
  if (!p)
    return DSERR_INVALIDPARAM;
  *p = buf(c)->live.cone_volume;
  return 0;
}
static int32_t API s_get_max(struct com *c, float *p) {
  if (!p)
    return DSERR_INVALIDPARAM;
  *p = buf(c)->live.maximum;
  return 0;
}
static int32_t API s_get_min(struct com *c, float *p) {
  if (!p)
    return DSERR_INVALIDPARAM;
  *p = buf(c)->live.minimum;
  return 0;
}
static int32_t API s_get_mode(struct com *c, uint32_t *p) {
  if (!p)
    return DSERR_INVALIDPARAM;
  *p = buf(c)->live.mode;
  return 0;
}
static int32_t API s_get_pos(struct com *c, struct vector *p) {
  if (!p)
    return DSERR_INVALIDPARAM;
  *p = buf(c)->live.position;
  return 0;
}
static int32_t API s_get_vel(struct com *c, struct vector *p) {
  if (!p)
    return DSERR_INVALIDPARAM;
  *p = buf(c)->live.velocity;
  return 0;
}
static int32_t API s_set_all(struct com *c, const struct spatial *p,
                             uint32_t apply) {
  if (!p || p->size != sizeof(*p) || apply > 1 || !valid_vec(p->position) ||
      !valid_vec(p->velocity) || !valid_vec(p->cone) || !finite(p->minimum) ||
      !finite(p->maximum) || p->minimum <= 0 || p->maximum < p->minimum ||
      p->mode > 2 || p->inside > 360 || p->outside > 360 ||
      p->cone_volume > 0 || p->cone_volume < -10000)
    return DSERR_INVALIDPARAM;
  CiukiAudioLock();
  struct buffer *b = buf(c);
  b->deferred = *p;
  b->dirty = 1;
  if (!apply) {
    b->live = *p;
    b->dirty = 0;
    spatial_gain(b);
  }
  CiukiAudioUnlock();
  return 0;
}
#define SPATIAL_VALUE(name, type, field)                                       \
  static int32_t API name(struct com *c, type v, uint32_t apply) {             \
    struct spatial p = apply ? buf(c)->deferred : buf(c)->live;                \
    p.field = v;                                                               \
    return s_set_all(c, &p, apply);                                            \
  }
SPATIAL_VALUE(s_set_max, float, maximum)
SPATIAL_VALUE(s_set_min, float, minimum)
SPATIAL_VALUE(s_set_mode, uint32_t, mode)
SPATIAL_VALUE(s_set_conevol, int32_t, cone_volume)
#define SPATIAL_VECTOR(name, field)                                            \
  static int32_t API name(struct com *c, float x, float y, float z,            \
                          uint32_t apply) {                                    \
    struct spatial p = apply ? buf(c)->deferred : buf(c)->live;                \
    p.field = (struct vector){x, y, z};                                        \
    return s_set_all(c, &p, apply);                                            \
  }
SPATIAL_VECTOR(s_set_pos, position)
SPATIAL_VECTOR(s_set_vel, velocity)
SPATIAL_VECTOR(s_set_cone, cone)
static int32_t API s_set_cones(struct com *c, uint32_t a, uint32_t b,
                               uint32_t apply) {
  struct spatial p = apply ? buf(c)->deferred : buf(c)->live;
  p.inside = a;
  p.outside = b;
  return s_set_all(c, &p, apply);
}
static int32_t API l_get_all(struct com *c, struct listener *p) {
  if (!p || p->size != sizeof(*p))
    return DSERR_INVALIDPARAM;
  *p = dev(c)->live;
  return 0;
}
#define LISTENER_GET(name, type, field)                                        \
  static int32_t API name(struct com *c, type *p) {                            \
    if (!p)                                                                    \
      return DSERR_INVALIDPARAM;                                               \
    *p = dev(c)->live.field;                                                   \
    return 0;                                                                  \
  }
LISTENER_GET(l_get_distance, float, distance)
LISTENER_GET(l_get_rolloff, float, rolloff)
LISTENER_GET(l_get_doppler, float, doppler)
LISTENER_GET(l_get_pos, struct vector, position)
LISTENER_GET(l_get_vel, struct vector, velocity)
static int32_t API l_get_orientation(struct com *c, struct vector *a,
                                     struct vector *b) {
  if (!a || !b)
    return DSERR_INVALIDPARAM;
  *a = dev(c)->live.front;
  *b = dev(c)->live.top;
  return 0;
}
static void update(struct device *d) {
  for (unsigned i = 0; i < WA_SOURCES; i++)
    if (buffers[i] && buffers[i]->device == d)
      spatial_gain(buffers[i]);
}
static int32_t API l_set_all(struct com *c, const struct listener *p,
                             uint32_t apply) {
  if (!p || p->size != sizeof(*p) || apply > 1 || !valid_vec(p->position) ||
      !valid_vec(p->velocity) || !valid_vec(p->front) || !valid_vec(p->top) ||
      !finite(p->distance) || !finite(p->rolloff) || !finite(p->doppler) ||
      p->distance <= 0 || p->rolloff < 0 || p->rolloff > 10 || p->doppler < 0 ||
      p->doppler > 10)
    return DSERR_INVALIDPARAM;
  CiukiAudioLock();
  struct device *d = dev(c);
  d->deferred = *p;
  d->dirty = 1;
  if (!apply) {
    d->live = *p;
    d->dirty = 0;
    update(d);
  }
  CiukiAudioUnlock();
  return 0;
}
#define LISTENER_VALUE(name, field)                                            \
  static int32_t API name(struct com *c, float v, uint32_t apply) {            \
    struct listener p = apply ? dev(c)->deferred : dev(c)->live;               \
    p.field = v;                                                               \
    return l_set_all(c, &p, apply);                                            \
  }
LISTENER_VALUE(l_set_distance, distance)
LISTENER_VALUE(l_set_rolloff, rolloff)
LISTENER_VALUE(l_set_doppler, doppler)
#define LISTENER_VECTOR(name, field)                                           \
  static int32_t API name(struct com *c, float x, float y, float z,            \
                          uint32_t apply) {                                    \
    struct listener p = apply ? dev(c)->deferred : dev(c)->live;               \
    p.field = (struct vector){x, y, z};                                        \
    return l_set_all(c, &p, apply);                                            \
  }
LISTENER_VECTOR(l_set_pos, position)
LISTENER_VECTOR(l_set_vel, velocity)
static int32_t API l_set_orientation(struct com *c, float x, float y, float z,
                                     float a, float b, float d,
                                     uint32_t apply) {
  struct listener p = apply ? dev(c)->deferred : dev(c)->live;
  p.front = (struct vector){x, y, z};
  p.top = (struct vector){a, b, d};
  return l_set_all(c, &p, apply);
}
static int32_t API commit(struct com *c) {
  CiukiAudioLock();
  struct device *d = dev(c);
  if (d->dirty) {
    d->live = d->deferred;
    d->dirty = 0;
  }
  for (unsigned i = 0; i < WA_SOURCES; i++)
    if (buffers[i] && buffers[i]->device == d && buffers[i]->dirty) {
      buffers[i]->live = buffers[i]->deferred;
      buffers[i]->dirty = 0;
    }
  update(d);
  CiukiAudioUnlock();
  return 0;
}
#define V(f) (const void *)(f)
static const void *const device_vt[] = {
    V(query),       V(add),         V(release),       V(create_buffer),
    V(ds_caps),     V(duplicate),   V(cooperative),   V(compact),
    V(get_speaker), V(set_speaker), V(ds_initialize), V(certify)};
static const void *const buffer_vt[] = {V(query),
                                        V(add),
                                        V(release),
                                        V(buffer_caps),
                                        V(position),
                                        V(get_format),
                                        V(get_volume),
                                        V(get_pan),
                                        V(get_frequency),
                                        V(status),
                                        V(buffer_initialize),
                                        V(lock_buffer),
                                        V(play),
                                        V(set_position),
                                        V(set_format),
                                        V(set_volume),
                                        V(set_pan),
                                        V(set_frequency),
                                        V(stop),
                                        V(unlock_buffer),
                                        V(restore),
                                        V(set_fx),
                                        V(acquire_resources),
                                        V(object_path)};
static const void *const notify_vt[] = {V(query), V(add), V(release),
                                        V(notifications)};
static const void *const spatial_vt[] = {
    V(query),       V(add),         V(release),       V(s_get_all),
    V(s_get_cones), V(s_get_cone),  V(s_get_conevol), V(s_get_max),
    V(s_get_min),   V(s_get_mode),  V(s_get_pos),     V(s_get_vel),
    V(s_set_all),   V(s_set_cones), V(s_set_cone),    V(s_set_conevol),
    V(s_set_max),   V(s_set_min),   V(s_set_mode),    V(s_set_pos),
    V(s_set_vel)};
static const void *const listener_vt[] = {V(query),
                                          V(add),
                                          V(release),
                                          V(l_get_all),
                                          V(l_get_distance),
                                          V(l_get_doppler),
                                          V(l_get_orientation),
                                          V(l_get_pos),
                                          V(l_get_rolloff),
                                          V(l_get_vel),
                                          V(l_set_all),
                                          V(l_set_distance),
                                          V(l_set_doppler),
                                          V(l_set_orientation),
                                          V(l_set_pos),
                                          V(l_set_rolloff),
                                          V(l_set_vel),
                                          V(commit)};
static const void *const ks_vt[] = {V(query),  V(add),    V(release),
                                    V(ks_get), V(ks_set), V(ks_support)};
int32_t API DirectSoundCreate(const struct guid *id, struct com **out,
                              void *outer) {
  (void)id;
  if (!out || outer)
    return DSERR_INVALIDPARAM;
  *out = 0;
  uint32_t error;
  struct wa_engine *e = CiukiAudioAcquire(44100, &error);
  if (!e)
    return error == 4 ? DSERR_ALLOCATED : DSERR_NODRIVER;
  CiukiAudioLock();
  struct device *d = wa_alloc(sizeof(*d));
  if (!d) {
    CiukiAudioUnlock();
    return E_OUTOFMEMORY;
  }
  d->com = (struct com){device_vt, d, 1};
  d->listener = (struct com){listener_vt, d, 2};
  d->refs = 1;
  d->coop = 1;
  d->speakers = 4;
  d->engine = e;
  d->live.size = sizeof(d->live);
  d->live.distance = d->live.rolloff = d->live.doppler = 1;
  d->live.front.z = 1;
  d->live.top.y = 1;
  d->deferred = d->live;
  *out = &d->com;
  CiukiAudioUnlock();
  return 0;
}
int32_t API DirectSoundCreate8(const struct guid *id, struct com **out,
                               void *outer) {
  return DirectSoundCreate(id, out, outer);
}
int32_t API DirectSoundEnumerateA(int(API *callback)(const struct guid *,
                                                     const char *, const char *,
                                                     void *),
                                  void *context) {
  if (!callback)
    return DSERR_INVALIDPARAM;
  callback(0, "CiukiOS audio", "CiukiOS", context);
  return 0;
}
int32_t API DirectSoundEnumerateW(int(API *callback)(const struct guid *,
                                                     const WCHAR *,
                                                     const WCHAR *, void *),
                                  void *context) {
  if (!callback)
    return DSERR_INVALIDPARAM;
  static const WCHAR name[] = {'C', 'i', 'u', 'k', 'i', 'O', 'S',
                               ' ', 'a', 'u', 'd', 'i', 'o', 0},
                     driver[] = {'C', 'i', 'u', 'k', 'i', 'O', 'S', 0};
  callback(0, name, driver, context);
  return 0;
}
int32_t API DirectSoundCaptureCreate(const struct guid *id, void **out,
                                     void *outer) {
  (void)id;
  (void)outer;
  if (out)
    *out = 0;
  return DSERR_NODRIVER;
}
int32_t API DirectSoundCaptureCreate8(const struct guid *id, void **out,
                                      void *outer) {
  return DirectSoundCaptureCreate(id, out, outer);
}
int32_t API DirectSoundCaptureEnumerateA(void *callback, void *context) {
  (void)callback;
  (void)context;
  return DSERR_NODRIVER;
}
int32_t API DirectSoundCaptureEnumerateW(void *callback, void *context) {
  return DirectSoundCaptureEnumerateA(callback, context);
}
