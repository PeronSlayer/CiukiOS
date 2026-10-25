/* Bounded command journal, immutable texture LRU and coherent scalar fallback.
 * This layer has no device names/registers. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
struct hw_texture {
  const struct sw_texture *source;
  uint64_t revision, age;
  struct sw_texture shadow;
  unsigned handle, bytes, pins;
};
struct hw_batch {
  struct hw_batch *next;
  unsigned submitted;
  struct hw_texture *textures[2];
  struct gl_hw_command command;
};
struct gl_hw_context {
  const struct gl_hw_driver *driver;
  struct gl_hw_caps caps;
  unsigned context, failed, loaded, bytes, batches;
  uint64_t age;
  struct hw_texture textures[GL_HW_TEXTURES];
  struct hw_batch *head, *tail;
};
static uint64_t texture_revision;
int gl_hw_active(const struct gl_context *c) {
  return c->hardware && c->hardware->context;
}
void gl_hw_texture_changed(struct sw_texture *t) {
  /* Saturation disables caching new versions rather than reusing an identity. */
  t->revision = texture_revision == UINT64_MAX ? 0 : ++texture_revision;
}
static const struct gl_hw_driver *driver(void) {
#ifdef W32_HOST_MODEL
  extern const struct gl_hw_driver *w32_gdi_render3d(unsigned) __attribute__((weak));
  const struct gl_hw_driver *d = w32_gdi_render3d ? w32_gdi_render3d(GL_HW_VERSION) : 0;
#else
  extern void *W32API GetModuleHandleA(const char *);
  extern void *W32API GetProcAddress(void *, const char *);
  static const struct gl_hw_driver *d;
  static unsigned tried;
  if (!tried) {
    tried = 1;
    const struct gl_hw_driver *(W32API *get)(unsigned) =
        GetProcAddress(GetModuleHandleA("gdi32.dll"), "w32_gdi_render3d");
    if (get) d = get(GL_HW_VERSION);
  }
#endif
  if (!d || d->version != GL_HW_VERSION || d->bytes != sizeof(*d) ||
      !d->query || !d->open || !d->close || !d->accept || !d->load || !d->read ||
      !d->texture || !d->free_texture || !d->submit || !d->finish)
    return 0;
  return d;
}
static void clear_scalar(const struct gl_hw_command *q) {
  if (!q->clear_mask) return;
  const struct sw_state *s = &q->state;
  for (unsigned y = 0; y < s->height; y++)
    for (unsigned x = 0; x < s->width; x++) {
      if (s->scissor && ((int)x < s->box[0] || (int)y < s->box[1] ||
          (int)x >= s->box[0] + s->box[2] || (int)y >= s->box[1] + s->box[3]))
        continue;
      unsigned p = (s->height - 1 - y) * s->width + x;
      if ((q->clear_mask & GL_COLOR_BUFFER_BIT) && !s->color_disabled) {
        s->color[p] = (s->color[p] & ~s->color_mask) | (q->clear_color & s->color_mask);
        if (s->color2)
          s->color2[p] = (s->color2[p] & ~s->color_mask) | (q->clear_color & s->color_mask);
      }
      if ((q->clear_mask & GL_DEPTH_BUFFER_BIT) && s->depth_write) {
        if (s->depth24) s->depth24[p] = q->clear_depth;
        else s->depth[p] = (uint16_t)q->clear_depth;
      }
      if ((q->clear_mask & GL_STENCIL_BUFFER_BIT) && s->stencil)
        s->stencil[p] = (s->stencil[p] & ~s->stencil_write_mask) |
                       (q->clear_stencil & s->stencil_write_mask);
    }
}
static void discard(struct gl_hw_context *h, int replay) {
  while (h->head) {
    struct hw_batch *b = h->head;
    if (replay) {
      struct sw_state *s = &b->command.state;
      clear_scalar(&b->command);
      for (unsigned i = 0; i < b->command.count; i += 3)
        sw_triangle(s, &b->command.vertices[i], &b->command.vertices[i+1],
                    &b->command.vertices[i+2]);
    }
    for (unsigned u = 0; u < 2; u++)
      if (b->textures[u]) b->textures[u]->pins--;
    h->head = b->next;
    gl_dealloc(b);
  }
  h->tail = 0; h->batches = 0;
}
static void evict(struct gl_hw_context *h, struct hw_texture *t) {
  if (!t->handle) return;
  h->driver->free_texture(h->context, t->handle);
  for (unsigned l = 0; l < SW_LEVELS; l++) gl_dealloc(t->shadow.image[l].pixels);
  h->bytes -= t->bytes;
  *t = (struct hw_texture){0};
}
static void fail(struct gl_hw_context *h) {
  /* close first: no engine may still access user staging or target storage. */
  h->driver->close(h->context);
  discard(h, 1);
  /* Provider close releases/quarantines handles; do not call them again. */
  for (unsigned i = 0; i < GL_HW_TEXTURES; i++) {
    struct hw_texture *t = &h->textures[i];
    for (unsigned l = 0; l < SW_LEVELS; l++) gl_dealloc(t->shadow.image[l].pixels);
    *t = (struct hw_texture){0};
  }
  h->context = h->bytes = h->loaded = 0;
  h->failed = 1;
}
void gl_hw_flush(struct gl_context *c) {
  struct gl_hw_context *h = c->hardware;
  if (!h || !h->context) return;
  for (struct hw_batch *b = h->head; b; b = b->next)
    if (!b->submitted) {
      if (h->driver->submit(h->context, &b->command)) { fail(h); return; }
      b->submitted = 1;
    }
}
void gl_hw_sync(struct gl_context *c) {
  struct gl_hw_context *h = c->hardware;
  if (!h || !h->head) return;
  gl_hw_flush(c);
  if (!h->context) return;
  /* A read failure must never partially overwrite the checkpoint. */
  struct sw_state s = h->head->command.state;
  unsigned pixels = s.width * s.height;
  unsigned bytes = pixels * (4 + (s.color2 ? 4 : 0) + (s.depth24 ? 4 : 2) + (s.stencil ? 1 : 0));
  uint8_t *staging = gl_alloc(bytes);
  if (!staging) { fail(h); return; }
  s.color = (uint32_t *)staging;
  unsigned at = pixels * 4;
  if (s.color2) { s.color2 = (uint32_t *)(staging + at); at += pixels * 4; }
  if (s.depth24) { s.depth24 = (uint32_t *)(staging + at); at += pixels * 4; }
  else { s.depth = (uint16_t *)(staging + at); at += pixels * 2; }
  if (s.stencil) s.stencil = staging + at;
  if (h->driver->finish(h->context) || h->driver->read(h->context, &s)) {
    gl_dealloc(staging); fail(h); return;
  }
  const struct sw_state *old = &h->head->command.state;
  w32_copy(old->color, s.color, pixels * 4);
  if (old->color2) w32_copy(old->color2, s.color2, pixels * 4);
  if (old->depth24) w32_copy(old->depth24, s.depth24, pixels * 4);
  else w32_copy(old->depth, s.depth, pixels * 2);
  if (old->stencil) w32_copy(old->stencil, s.stencil, pixels);
  gl_dealloc(staging);
  discard(h, 0);
}
void gl_hw_software(struct gl_context *c) {
  gl_hw_sync(c);
  if (c->hardware) c->hardware->loaded = 0;
}
void gl_hw_close(struct gl_context *c) {
  struct gl_hw_context *h = c->hardware;
  if (!h) return;
  gl_hw_sync(c);
  if (h->context) {
    for (unsigned i = 0; i < GL_HW_TEXTURES; i++) evict(h, &h->textures[i]);
    h->driver->close(h->context);
  }
  gl_dealloc(h);
  c->hardware = 0;
}
void gl_hw_probe(struct gl_context *c) {
  const struct gl_hw_driver *d = driver();
  if (!d) { gl_hw_close(c); return; }
  struct gl_hw_format f = {c->sw.width, c->sw.height, c->depth_bits,
                           c->stencil_bits, (unsigned)c->sw.rgb565};
  struct gl_hw_caps caps = {0};
  if (!d->query(c->dc, &f, &caps) || caps.version != GL_HW_VERSION || !caps.generation ||
      caps.max_vertices < 3 || caps.max_vertices > GL_HW_VERTICES ||
      caps.texture_units > 2 || caps.max_size < f.width || caps.max_size < f.height ||
      caps.texture_bytes > 32u * 1024u * 1024u) {
    gl_hw_close(c); return;
  }
  if (c->hardware && (c->hardware->driver != d ||
      memcmp(&c->hardware->caps, &caps, sizeof(caps)))) gl_hw_close(c);
  if (!c->hardware) {
    struct gl_hw_context *h = gl_alloc(sizeof(*h));
    if (!h) return;
    h->driver = d; h->caps = caps;
    c->hardware = h;
    if (d->open(c->dc, &f, &h->context) || !h->context) {
      if (h->context) d->close(h->context);
      h->context = 0; h->failed = 1;
    }
  }
}
static int ready(struct gl_context *c) {
  struct gl_hw_context *h = c->hardware;
  if (!h || !h->context) return 0;
  if (!h->loaded) {
    if (h->driver->load(h->context, &c->sw)) { fail(h); return 0; }
    h->loaded = 1;
  }
  return 1;
}
static struct hw_texture *resident(struct gl_context *c, struct sw_texture *source) {
  struct gl_hw_context *h = c->hardware;
  if (!source->revision) gl_hw_texture_changed(source);
  if (!source->revision) return 0;
  for (unsigned i = 0; i < GL_HW_TEXTURES; i++) {
    struct hw_texture *t = &h->textures[i];
    if (t->handle && t->source == source && t->revision == source->revision) {
      t->age = ++h->age; t->pins++; return t;
    }
  }
  unsigned bytes = 0;
  for (unsigned l = 0; l < SW_LEVELS; l++) {
    const struct sw_image *m = &source->image[l];
    if (m->w > SW_MAX_SIZE || m->h > SW_MAX_SIZE) return 0;
    if (m->pixels) bytes += m->w * m->h * 4;
  }
  if (!bytes || bytes > h->caps.texture_bytes) return 0;
  struct hw_texture *slot = 0;
  for (;;) {
    struct hw_texture *old = 0;
    for (unsigned i = 0; i < GL_HW_TEXTURES; i++) {
      struct hw_texture *t = &h->textures[i];
      if (!t->handle) slot = t;
      else if (!t->pins && (!old || t->age < old->age)) old = t;
    }
    if (slot && bytes <= h->caps.texture_bytes - h->bytes) break;
    if (old) { evict(h, old); continue; }
    /* All candidates are used by the journal. Read before dropping versions. */
    if (!h->head) return 0;
    gl_hw_sync(c);
    if (!h->context) return 0;
  }
  struct sw_texture shadow = *source;
  for (unsigned l = 0; l < SW_LEVELS; l++) {
    shadow.image[l].pixels = 0;
    shadow.image[l].compressed = 0;
  }
  for (unsigned l = 0; l < SW_LEVELS; l++) {
    const struct sw_image *m = &source->image[l];
    if (m->pixels) {
      unsigned size = m->w * m->h * 4;
      shadow.image[l].pixels = gl_alloc(size);
      if (!shadow.image[l].pixels) goto rejected;
      w32_copy(shadow.image[l].pixels, m->pixels, size);
    }
  }
  unsigned handle = 0;
  if (h->driver->texture(h->context, &shadow, &handle) || !handle) {
    if (handle) h->driver->free_texture(h->context, handle);
    goto rejected;
  }
  *slot = (struct hw_texture){source, source->revision, ++h->age, shadow, handle, bytes, 1};
  h->bytes += bytes;
  return slot;
rejected:
  for (unsigned l = 0; l < SW_LEVELS; l++) gl_dealloc(shadow.image[l].pixels);
  return 0;
}
static struct hw_batch *batch(struct gl_context *c, unsigned mask,
                              const struct sw_vertex *v) {
  struct gl_hw_context *h = c->hardware;
  if (!h || !h->context || !h->driver->accept(h->context, &c->sw, mask, v, v ? 3 : 0))
    return 0;
  if (h->batches == GL_HW_BATCHES) gl_hw_sync(c);
  if (!ready(c)) return 0;
  struct sw_state state = c->sw;
  struct hw_texture *textures[2] = {0};
  for (unsigned u = 0; u < 2; u++) {
    if (v && state.unit[u].enabled) {
      if (u >= h->caps.texture_units || !(textures[u] = resident(c, state.unit[u].texture)))
        goto rejected;
      state.unit[u].texture = &textures[u]->shadow;
    } else state.unit[u].texture = 0;
  }
  struct hw_batch *b = h->tail;
  if (v && b && !b->submitted && !b->command.clear_mask &&
      b->command.count + 3 <= h->caps.max_vertices &&
      !memcmp(&b->command.state, &state, sizeof(state))) {
    for (unsigned u = 0; u < 2; u++) if (textures[u]) textures[u]->pins--;
    return b;
  }
  b = gl_alloc(sizeof(*b));
  if (!b) goto rejected;
  b->command.state = state;
  for (unsigned u = 0; u < 2; u++) {
    b->textures[u] = textures[u];
    b->command.texture[u] = textures[u] ? textures[u]->handle : 0;
  }
  if (h->tail) h->tail->next = b;
  else h->head = b;
  h->tail = b; h->batches++;
  return b;
rejected:
  for (unsigned u = 0; u < 2; u++)
    if (textures[u] && textures[u]->pins) textures[u]->pins--;
  return 0;
}
int gl_hw_triangle(struct gl_context *c, const struct sw_vertex *v) {
  struct hw_batch *b = batch(c, 0, v);
  if (!b) { gl_hw_software(c); return 0; }
  w32_copy(&b->command.vertices[b->command.count], v, 3 * sizeof(*v));
  b->command.count += 3;
  return 1;
}
int gl_hw_clear(struct gl_context *c, unsigned mask, unsigned color, unsigned depth) {
  /* Accumulation is outside this backend's semantics. */
  if (!mask || (mask & GL_ACCUM_BUFFER_BIT)) { gl_hw_software(c); return 0; }
  struct hw_batch *b = batch(c, mask, 0);
  if (!b) { gl_hw_software(c); return 0; }
  b->command.clear_mask = mask; b->command.clear_color = color;
  b->command.clear_depth = depth; b->command.clear_stencil = c->clear_stencil;
  return 1;
}
int gl_hw_present(struct gl_context *c, unsigned token, unsigned generation,
                   unsigned width, unsigned height, unsigned bpp, unsigned pitch) {
  struct gl_hw_context *h = c->hardware;
  if (!h || !h->context || !h->loaded || !h->driver->present) return 0;
  gl_hw_flush(c);
  if (!h->context) return 0;
  if (h->driver->finish(h->context)) { fail(h); return 0; }
  return h->driver->present(h->context, token, generation, width, height, bpp, pitch);
}
