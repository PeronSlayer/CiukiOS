/* Bounded command journal, immutable texture LRU and coherent scalar fallback.
 * This layer has no device names/registers. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
#include "admission.h"
struct hw_texture {
  const struct sw_texture *source;
  uint64_t revision, age;
  struct sw_texture shadow;
  unsigned handle, bytes, pins;
};
struct hw_batch {
  struct hw_batch *next;
  unsigned submitted;
  uint64_t work;
  struct hw_texture *textures[2];
  struct gl_hw_command command;
};
struct gl_hw_context {
  const struct gl_hw_driver *driver;
  struct gl_hw_caps caps;
  unsigned context, loaded, bytes, batches;
  /* GPU-resident attachments (f7-15 lead decision): RAM is a checkpoint that
   * is read back only when the CPU needs the pixels. gpu_only: the journal
   * was dropped (full) without a readback, so RAM is stale until the next
   * sync. presented/front_pending: the last swap presented the GPU back
   * buffer; the front buffer is that completed copy, read on demand. */
  unsigned gpu_only, presented, front_pending;
  struct sw_state target; /* RAM attachments that the GPU copy mirrors */
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
static void lose(struct gl_context *c, int permanent) {
  struct gl_hw_context *h = c->hardware;
  if (permanent) c->hardware_failed = 1;
  /* close first: no engine may still access user staging or target storage. */
  h->driver->close(h->context);
  /* Replay onto the RAM checkpoint. After a gpu_only span that checkpoint
   * is older than the journal: only a healthy refusal reads back first. */
  discard(h, 1);
  /* Provider close releases/quarantines handles; do not call them again. */
  for (unsigned i = 0; i < GL_HW_TEXTURES; i++) {
    struct hw_texture *t = &h->textures[i];
    for (unsigned l = 0; l < SW_LEVELS; l++) gl_dealloc(t->shadow.image[l].pixels);
    *t = (struct hw_texture){0};
  }
  h->context = h->bytes = h->loaded = 0;
  h->gpu_only = h->presented = h->front_pending = 0;
}
/* A display mode change gives the kernel a new generation and revokes this
 * context: not a hardware failure, so probe may admit a fresh context (the
 * full-screen lease of Quake III switches mode at its first swap). */
static int revoked(struct gl_context *c) {
  struct gl_hw_context *h = c->hardware;
  struct gl_hw_format f = {c->sw.width, c->sw.height, c->depth_bits,
                           c->stencil_bits, (unsigned)c->sw.rgb565};
  struct gl_hw_caps caps = {0};
  return !h->driver->query(c->dc, &f, &caps) || caps.generation != h->caps.generation;
}
static void fail(struct gl_context *c) { lose(c, !revoked(c)); }
static void materialize_front(struct gl_context *);
/* A semantic refusal on a healthy context: make RAM exact before the scalar
 * replay takes over (the GPU copy may be newer than the checkpoint). */
static void refuse(struct gl_context *c) {
  struct gl_hw_context *h = c->hardware;
  materialize_front(c);
  if (h->context && h->gpu_only) gl_hw_sync(c);
  if (h->context) lose(c, 1);
}
void gl_hw_flush(struct gl_context *c) {
  struct gl_hw_context *h = c->hardware;
  if (!h || !h->context) return;
  for (struct hw_batch *b = h->head; b; b = b->next)
    if (!b->submitted) {
      if (h->driver->submit(h->context, &b->command)) { fail(c); return; }
      b->submitted = 1;
    }
}
static int front_needed(const struct gl_context *c) {
  return c->doublebuffer &&
    ((c->draw_buffer != GL_BACK && c->draw_buffer != GL_BACK_LEFT && c->draw_buffer != GL_NONE) ||
     c->read_buffer == GL_FRONT || c->read_buffer == GL_FRONT_LEFT || c->read_buffer == GL_LEFT);
}
/* The front buffer of a GPU swap is the provider's completed presentation
 * copy; it is read only when the CPU reads, draws or presents the front. */
static void materialize_front(struct gl_context *c) {
  struct gl_hw_context *h = c->hardware;
  if (!h || !h->front_pending) return;
  h->front_pending = 0;
  if (h->context && h->driver->read_presented &&
      h->driver->read_presented(h->context, c->front_color, c->sw.width, c->sw.height))
    fail(c);
}
void gl_hw_sync(struct gl_context *c) {
  struct gl_hw_context *h = c->hardware;
  if (!h) return;
  h->presented = 0; /* a CPU present follows, never the GPU swap copy */
  if (front_needed(c)) materialize_front(c);
  if (!h->context || (!h->head && !h->gpu_only)) return;
  gl_hw_flush(c);
  if (!h->context) return;
  /* A read failure must never partially overwrite the checkpoint. */
  struct sw_state s = h->head ? h->head->command.state : h->target;
  unsigned pixels = s.width * s.height;
  unsigned bytes = pixels * (4 + (s.color2 ? 4 : 0) + (s.depth24 ? 4 : 2) + (s.stencil ? 1 : 0));
  uint8_t *staging = gl_alloc(bytes);
  if (!staging) { fail(c); return; }
  s.color = (uint32_t *)staging;
  unsigned at = pixels * 4;
  if (s.color2) { s.color2 = (uint32_t *)(staging + at); at += pixels * 4; }
  if (s.depth24) { s.depth24 = (uint32_t *)(staging + at); at += pixels * 4; }
  else { s.depth = (uint16_t *)(staging + at); at += pixels * 2; }
  if (s.stencil) s.stencil = staging + at;
  if (h->driver->finish(h->context) || h->driver->read(h->context, &s)) {
    gl_dealloc(staging); fail(c); return;
  }
  const struct sw_state *old = h->head ? &h->head->command.state : &h->target;
  w32_copy(old->color, s.color, pixels * 4);
  if (old->color2) w32_copy(old->color2, s.color2, pixels * 4);
  if (old->depth24) w32_copy(old->depth24, s.depth24, pixels * 4);
  else w32_copy(old->depth, s.depth, pixels * 2);
  if (old->stencil) w32_copy(old->stencil, s.stencil, pixels);
  gl_dealloc(staging);
  discard(h, 0);
  h->gpu_only = 0;
}
void gl_hw_coherent(struct gl_context *c) {
  materialize_front(c);
  gl_hw_sync(c);
}
/* glFinish: completion. A bitmap/memory DC exposes the attachment memory to
 * the application, so it is made coherent; a window's store is private and
 * presentation reads it back only when presentation itself needs the CPU. */
void gl_hw_finish(struct gl_context *c) {
  struct gl_hw_context *h = c->hardware;
  unsigned w = 0, hh = 0, window = 0;
  if (!h || !h->context) return;
  if (!gl_target(c->dc, &w, &hh, &window) || !window) { gl_hw_sync(c); return; }
  gl_hw_flush(c);
  if (h->context && h->driver->finish(h->context)) fail(c);
}
int gl_hw_swap_front(struct gl_context *c) {
  struct gl_hw_context *h = c->hardware;
  if (!h) return 0;
  int gpu = h->context && h->presented && h->driver->read_presented;
  h->presented = 0; h->front_pending = gpu;
  return gpu;
}
void gl_hw_software(struct gl_context *c) {
  gl_hw_sync(c);
  if (c->hardware) c->hardware->loaded = 0;
}
void gl_hw_close(struct gl_context *c) {
  struct gl_hw_context *h = c->hardware;
  if (!h) return;
  gl_hw_coherent(c);
  if (h->context) {
    for (unsigned i = 0; i < GL_HW_TEXTURES; i++) evict(h, &h->textures[i]);
    h->driver->close(h->context);
  }
  gl_dealloc(h);
  c->hardware = 0;
}
void gl_hw_probe(struct gl_context *c) {
  if (c->hardware_failed) return;
  const struct gl_hw_driver *d = driver();
  if (!d) { gl_hw_close(c); return; }
  struct gl_hw_format f = {c->sw.width, c->sw.height, c->depth_bits,
                           c->stencil_bits, (unsigned)c->sw.rgb565};
  struct gl_hw_caps caps = {0};
  if (!d->query(c->dc, &f, &caps) || caps.version != GL_HW_VERSION || !caps.generation ||
      caps.max_vertices < 3 || caps.max_vertices > GL_HW_VERTICES ||
      !caps.max_pixel_work ||
      caps.texture_units > 2 || caps.max_size < f.width || caps.max_size < f.height ||
      caps.texture_bytes > 32u * 1024u * 1024u) {
    gl_hw_close(c); return;
  }
  if (c->hardware && (c->hardware->driver != d ||
      memcmp(&c->hardware->caps, &caps, sizeof(caps)))) {
    /* Checkpoint the old session if it still answers; a new generation
     * that revoked it fails the reads, which fail() treats as revocation
     * (replay, no permanent mark), never as a hardware failure. */
    gl_hw_close(c);
  }
  if (!c->hardware) {
    struct gl_hw_context *h = gl_alloc(sizeof(*h));
    if (!h) return;
    h->driver = d; h->caps = caps;
    c->hardware = h;
    if (d->open(c->dc, &f, &h->context) || !h->context) {
      if (h->context) d->close(h->context);
      h->context = 0; c->hardware_failed = 1;
    }
  }
}
static int ready(struct gl_context *c, int full) {
  struct gl_hw_context *h = c->hardware;
  if (!h || !h->context) return 0;
  /* A command that overwrites every attachment needs no upload of RAM. */
  if (!h->loaded && !full) {
    if (h->driver->load(h->context, &c->sw)) { fail(c); return 0; }
  }
  if (!h->loaded) h->target = c->sw;
  h->loaded = 1;
  return 1;
}
/* glClear of every attachment of the current target, unmasked and
 * unscissored: the frame no longer depends on any earlier content. */
static int full_clear(const struct gl_context *c, unsigned mask) {
  const struct sw_state *s = &c->sw;
  if (!(mask & GL_COLOR_BUFFER_BIT) || s->color_disabled || s->color2 || s->color_mask != 0xffffffffu)
    return 0;
  if (s->scissor && (s->box[0] > 0 || s->box[1] > 0 || s->box[0] + s->box[2] < (int)s->width ||
                     s->box[1] + s->box[3] < (int)s->height))
    return 0;
  if ((s->depth || s->depth24) && (!(mask & GL_DEPTH_BUFFER_BIT) || !s->depth_write)) return 0;
  if (s->stencil && (!(mask & GL_STENCIL_BUFFER_BIT) || (s->stencil_write_mask & 255u) != 255u)) return 0;
  return 1;
}
/* A full journal is submitted and dropped without a readback: the GPU copy
 * stays authoritative (gpu_only) until the CPU needs the pixels. */
static void drop_journal(struct gl_context *c) {
  struct gl_hw_context *h = c->hardware;
  gl_hw_flush(c);
  if (!h->context) return;
  discard(h, 0);
  h->gpu_only = 1;
}
/* After a full clear, earlier commands are dead and the journal is a
 * complete description from any base: no readback, nothing to replay. */
static void rebase(struct gl_hw_context *h, struct hw_batch *keep) {
  while (h->head && h->head != keep) {
    struct hw_batch *b = h->head;
    for (unsigned u = 0; u < 2; u++)
      if (b->textures[u]) b->textures[u]->pins--;
    h->head = b->next; h->batches--;
    gl_dealloc(b);
  }
  h->gpu_only = 0;
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
  if (!h || !h->context) return 0;
  int accepted=h->driver->accept(h->context, &c->sw, mask, v, v ? 3 : 0);
  if (accepted<0) { refuse(c); return 0; }
  if (!accepted) return 0;
  uint64_t work=v ? gl_hw_triangle_work(v) : 0;
  if (v && !gl_hw_batch_room(&h->caps,0,0,work)) { refuse(c); return 0; }
  if (h->batches == GL_HW_BATCHES) drop_journal(c);
  if (!ready(c, !v && full_clear(c, mask))) return 0;
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
      gl_hw_batch_room(&h->caps,b->command.count,b->work,work) &&
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
  b->work += gl_hw_triangle_work(v);
  return 1;
}
int gl_hw_clear(struct gl_context *c, unsigned mask, unsigned color, unsigned depth) {
  if (c->census) gl_census_clear(c,mask);
  /* Accumulation is outside this backend's semantics. */
  if (!mask || (mask & GL_ACCUM_BUFFER_BIT)) { gl_hw_software(c); return 0; }
  struct hw_batch *b = batch(c, mask, 0);
  if (!b) { gl_hw_software(c); return 0; }
  b->command.clear_mask = mask; b->command.clear_color = color;
  b->command.clear_depth = depth; b->command.clear_stencil = c->clear_stencil;
  if (full_clear(c, mask)) rebase(c->hardware, b);
  return 1;
}
int gl_hw_present(struct gl_context *c, unsigned token, unsigned generation,
                   unsigned width, unsigned height, unsigned bpp, unsigned pitch) {
  struct gl_hw_context *h = c->hardware;
  if (!h || !h->context || !h->loaded || !h->driver->present) return 0;
  /* Draw selection is independent of swap's back-buffer source. Selection
   * changes invalidate loaded, so a loaded target always matches sw.color. */
  if (c->sw.color != c->back_color) return 0;
  gl_hw_flush(c);
  if (!h->context) return 0;
  if (h->driver->finish(h->context)) { fail(c); return 0; }
  int presented = h->driver->present(h->context, token, generation, width, height, bpp, pitch);
  /* GPU frame: no readback; the swap records the front buffer lazily. */
  h->presented = presented > 0;
  return presented;
}
