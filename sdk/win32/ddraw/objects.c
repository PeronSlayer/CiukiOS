/* Original COM identity and auxiliary objects. SPDX-License-Identifier:
 * LGPL-2.1-or-later */
#include "internal.h"
#define LEGACY_GUID(a)                                                         \
  {a, 0xa733, 0x11ce, {0xa5, 0x21, 0, 0x20, 0xaf, 0x0b, 0xe5, 0x60}}
const DDGUID dd_unknown_iid = {0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
const DDGUID dd_iids[4] = {
    LEGACY_GUID(0x6c14db80),
    {0xb3a6f3e0, 0x2b43, 0x11cf, {0xa2, 0xde, 0, 0xaa, 0, 0xb9, 0x33, 0x56}},
    {0x9c59509a, 0x39bd, 0x11d1, {0x8c, 0x4a, 0, 0xc0, 0x4f, 0xd9, 0x30, 0xc5}},
    {0x15e65ec0,
     0x3b9c,
     0x11d2,
     {0xb9, 0x2f, 0, 0x60, 0x97, 0x97, 0xea, 0x5b}}};
const DDGUID dd_surface_iids[5] = {
    LEGACY_GUID(0x6c14db81),
    {0x57805885, 0x6eec, 0x11cf, {0x94, 0x41, 0xa8, 0x23, 3, 0xc1, 0x0e, 0x27}},
    {0xda044e00, 0x69b2, 0x11d0, {0xa1, 0xd5, 0, 0xaa, 0, 0xb8, 0xdf, 0xbb}},
    {0x0b2b8630, 0xad35, 0x11d0, {0x8e, 0xa6, 0, 0x60, 0x97, 0x97, 0xea, 0x5b}},
    {0x06675a80,
     0x3b9b,
     0x11d2,
     {0xb9, 0x2f, 0, 0x60, 0x97, 0x97, 0xea, 0x5b}}};
const DDGUID dd_palette_iid = LEGACY_GUID(0x6c14db84),
             dd_clipper_iid = LEGACY_GUID(0x6c14db85);
const DDGUID dd_gamma_iid = {
    0x69c11c3e, 0xb46b, 0x11d1, {0xad, 0x7a, 0, 0xc0, 0x4f, 0xc2, 0x9b, 0x4e}};
const DDGUID dd_color_iid = {
    0x4b9f0ee0, 0x0d7e, 0x11d0, {0x9b, 6, 0, 0xa0, 0xc9, 3, 0xa3, 0xb8}};
const struct dd_backend *dd_backend = &dd_native_backend;
struct dd_device *dd_devices;
DWORD dd_bytes, dd_budget;
static struct dd_object *objects;
static volatile DWORD lock_owner;
static DWORD lock_depth;
void dd_lock(void) {
  const struct w32_services *s = w32_get_services();
  DWORD tid = s->thread_id(s->opaque);
  if (__atomic_load_n(&lock_owner, __ATOMIC_ACQUIRE) == tid) {
    lock_depth++;
    return;
  }
  while (!__sync_bool_compare_and_swap(&lock_owner, 0, tid)) {
    DWORD observed = __atomic_load_n(&lock_owner, __ATOMIC_RELAXED);
    if (observed)
      s->wait_word(s->opaque, &lock_owner, observed);
  }
  lock_depth = 1;
}
void dd_unlock(int *unused) {
  (void)unused;
  if (!--lock_depth) {
    const struct w32_services *s = w32_get_services();
    __atomic_store_n(&lock_owner, 0, __ATOMIC_RELEASE);
    s->wake_word(s->opaque, &lock_owner);
  }
}
DWORD dd_suspend(void) {
  const struct w32_services *s = w32_get_services();
  if (__atomic_load_n(&lock_owner, __ATOMIC_ACQUIRE) != s->thread_id(s->opaque))
    return 0;
  DWORD depth = lock_depth;
  lock_depth = 0;
  __atomic_store_n(&lock_owner, 0, __ATOMIC_RELEASE);
  s->wake_word(s->opaque, &lock_owner);
  return depth;
}
void dd_resume(DWORD depth) {
  if (depth) {
    dd_lock();
    lock_depth = depth;
  }
}
void *dd_alloc(DWORD n) {
  if (n > 0x7ffffff0u)
    return 0;
  const struct w32_services *s = w32_get_services();
  DWORD *p = s->alloc(s->opaque, n + 8);
  if (!p)
    return 0;
  p[0] = n + 8;
  p[1] = 0;
  w32_fill(p + 2, 0, n);
  return p + 2;
}
void dd_free(void *v) {
  if (!v)
    return;
  const struct w32_services *s = w32_get_services();
  DWORD *p = (DWORD *)v - 2;
  s->free(s->opaque, p, p[0]);
}
BOOL dd_equal(const DDGUID *a, const DDGUID *b) {
  if (!a || !b)
    return 0;
  const uint8_t *x = (const void *)a, *y = (const void *)b;
  for (unsigned i = 0; i < 16; i++)
    if (x[i] != y[i])
      return 0;
  return 1;
}
void dd_init(struct dd_object *o, DWORD kind, DWORD bytes,
             const DWORD *generations, const void *const *const *tables,
             DWORD count) {
  o->next = objects;
  objects = o;
  o->kind = kind;
  o->refs = 1;
  o->bytes = bytes;
  for (DWORD i = 0; i < count; i++)
    o->interfaces[i] = (DD){tables[i], o, generations[i], kind};
}
DDRESULT API dd_query(DD *c, const DDGUID *id, void **out) {
  GUARD;
  if (!out || !id)
    return DDERR_INVALIDPARAMS;
  *out = 0;
  struct dd_object *o = c->owner;
  if (dd_equal(id, &dd_unknown_iid))
    *out = &o->interfaces[0];
  if (o->kind == 1)
    for (unsigned i = 0; i < 4; i++)
      if (dd_equal(id, &dd_iids[i]))
        *out = &o->interfaces[i];
  if (o->kind == 2) {
    for (unsigned i = 0; i < 5; i++)
      if (dd_equal(id, &dd_surface_iids[i]))
        *out = &o->interfaces[i];
    if (dd_equal(id, &dd_gamma_iid))
      *out = &o->interfaces[5];
    if (dd_equal(id, &dd_color_iid))
      *out = &o->interfaces[6];
  }
  if ((o->kind == 3 && dd_equal(id, &dd_palette_iid)) ||
      (o->kind == 4 && dd_equal(id, &dd_clipper_iid)))
    *out = &o->interfaces[0];
  if (!*out)
    return (DDRESULT)0x80004002u;
  o->refs++;
  return DD_OK;
}
DWORD API dd_add(DD *c) {
  GUARD;
  return ++((struct dd_object *)c->owner)->refs;
}
DWORD API dd_release(DD *c) {
  GUARD;
  struct dd_object *o = c->owner;
  DWORD refs = --o->refs;
  if (refs)
    return refs;
  struct dd_object **slot = &objects;
  while (*slot && *slot != o)
    slot = &(*slot)->next;
  if (*slot)
    *slot = o->next;
  if (o->kind == 1) {
    struct dd_device *d = (void *)o;
    if ((d->mode_set || (d->coop & DDSCL_EXCLUSIVE)) && dd_backend->release)
      dd_backend->release(d->hwnd);
    struct dd_device **p = &dd_devices;
    while (*p && *p != d)
      p = &(*p)->next;
    if (*p)
      *p = d->next;
    dd_device_finalized(d);
  } else if (o->kind == 2)
    dd_surface_destroy((void *)o);
  else if (o->kind == 4)
    dd_free(((struct dd_clipper *)o)->rects);
  dd_free(o);
  return 0;
}
void API w32_ddraw_set_backend(const struct dd_backend *b) {
  GUARD;
  if (!dd_devices)
    dd_backend = b ? b : &dd_native_backend;
}
DWORD API w32_ddraw_surface_bytes(void) {
  GUARD;
  return dd_bytes;
}
void API w32_ddraw_tick(void) {
  GUARD;
  struct dd_device *d = dd_devices;
  if (d)
    dd_add(&d->base.interfaces[0]);
  while (d) {
    dd_sync(d);
    if (d->dirty && d->primary && !d->primary->locked && !d->primary->dc &&
        dd_backend->clock_ns() >= d->next_frame)
      dd_present(d->primary, 0, 1);
    struct dd_device *next = d->next;
    if (next)
      dd_add(&next->base.interfaces[0]);
    dd_release(&d->base.interfaces[0]);
    d = next;
  }
}
void API w32_ddraw_process_exit(void) {
  if (dd_backend == &dd_native_backend)
    dd_native_stop();
  GUARD;
  /* Child references are broken before releasing the device. DLL detach owns
   * the process; application interface pointers are no longer usable. */
  while (dd_devices) {
    struct dd_device *d = dd_devices;
    d->base.refs++;
    for (struct dd_surface *s = d->surfaces; s; s = s->next)
      for (DWORD i = 0; i < s->count; i++)
        s->attached[i] = 0;
    while (d->surfaces) {
      struct dd_surface *s = d->surfaces;
      s->count = 0;
      s->base.refs = 1;
      dd_release(&s->base.interfaces[0]);
    }
    d->base.refs = 1;
    dd_release(&d->base.interfaces[0]);
  }
  while (objects) {
    objects->refs = 1;
    dd_release(&objects->interfaces[0]);
  }
}
static DDRESULT API palette_caps(DD *c, DWORD *out) {
  GUARD;
  if (!out)
    return DDERR_INVALIDPARAMS;
  *out = ((struct dd_palette *)c->owner)->flags;
  return 0;
}
static DDRESULT API palette_get(DD *c, DWORD flags, DWORD first, DWORD count,
                                PALETTEENTRY *out) {
  GUARD;
  if (flags || !out || first > 256 || count > 256 - first)
    return DDERR_INVALIDPARAMS;
  w32_copy(out, ((struct dd_palette *)c->owner)->colors + first, count * 4);
  return 0;
}
static DDRESULT API palette_init(DD *c, DD *d, DWORD f, PALETTEENTRY *p) {
  (void)c;
  (void)d;
  (void)f;
  (void)p;
  return DDERR(5);
}
static DDRESULT API palette_set(DD *c, DWORD flags, DWORD first, DWORD count,
                                PALETTEENTRY *in) {
  GUARD;
  if (flags || !in || first > 256 || count > 256 - first)
    return DDERR_INVALIDPARAMS;
  struct dd_palette *p = c->owner;
  w32_copy(p->colors + first, in, count * 4);
  dd_palette_update(p, first, count);
  return 0;
}
const void *const dd_palette_vt[] = {dd_query,     dd_add,      dd_release,
                                     palette_caps, palette_get, palette_init,
                                     palette_set};
DDRESULT dd_create_palette(DD *c, DWORD flags, PALETTEENTRY *entries, DD **out,
                           void *outer) {
  (void)c;
  if (!out)
    return DDERR_INVALIDPARAMS;
  *out = 0;
  if (outer)
    return (DDRESULT)0x80040110u;
  if (!entries || !(flags & DDPCAPS_8BIT) ||
      (flags & ~(4u | 0x10u | 0x20u | 0x40u | 0x80u)))
    return DDERR_INVALIDPARAMS;
  struct dd_palette *p = dd_alloc(sizeof(*p));
  if (!p)
    return DDERR_OUTOFMEMORY;
  DWORD gen = 1;
  const void *const *vt = dd_palette_vt;
  dd_init(&p->base, 3, sizeof(*p), &gen, &vt, 1);
  p->flags = flags;
  w32_copy(p->colors, entries, 1024);
  *out = &p->base.interfaces[0];
  return 0;
}
DDRESULT dd_clip_rects(struct dd_clipper *c, RECT *out, DWORD *count) {
  if (!c || !count)
    return DDERR_NOCLIPLIST;
  if (c->hwnd)
    return dd_backend->clip ? dd_backend->clip(c->hwnd, out, count)
                            : DDERR_NOCLIPLIST;
  if (!c->rects)
    return DDERR_NOCLIPLIST;
  if (!out) {
    *count = c->count;
    return 0;
  }
  if (*count < c->count) {
    *count = c->count;
    return DDERR_MOREDATA;
  }
  w32_copy(out, c->rects, c->count * sizeof(RECT));
  *count = c->count;
  return 0;
}
static DDRESULT API clip_list(DD *c, RECT *limit, RGNDATA *out, DWORD *bytes) {
  GUARD;
  if (!bytes)
    return DDERR_INVALIDPARAMS;
  RECT rects[DD_MAX_RECTS];
  DWORD n = DD_MAX_RECTS;
  DDRESULT r = dd_clip_rects(c->owner, rects, &n);
  if (r)
    return r;
  DWORD used = 0;
  RECT bounds = {0};
  for (DWORD i = 0; i < n; i++) {
    RECT v = rects[i];
    if (limit) {
      if (v.left < limit->left)
        v.left = limit->left;
      if (v.top < limit->top)
        v.top = limit->top;
      if (v.right > limit->right)
        v.right = limit->right;
      if (v.bottom > limit->bottom)
        v.bottom = limit->bottom;
    }
    if (v.right <= v.left || v.bottom <= v.top)
      continue;
    if (!used)
      bounds = v;
    else {
      if (v.left < bounds.left)
        bounds.left = v.left;
      if (v.top < bounds.top)
        bounds.top = v.top;
      if (v.right > bounds.right)
        bounds.right = v.right;
      if (v.bottom > bounds.bottom)
        bounds.bottom = v.bottom;
    }
    rects[used++] = v;
  }
  DWORD need = sizeof(RGNDATAHEADER) + used * sizeof(RECT), capacity = *bytes;
  *bytes = need;
  if (!out)
    return 0;
  if (capacity < need)
    return DDERR_MOREDATA;
  out->rdh = (RGNDATAHEADER){32, 1, used, used * sizeof(RECT), bounds};
  w32_copy(out->Buffer, rects, used * sizeof(RECT));
  return 0;
}
static DDRESULT API clip_hwnd(DD *c, uintptr_t *out) {
  GUARD;
  if (!out)
    return DDERR_INVALIDPARAMS;
  *out = ((struct dd_clipper *)c->owner)->hwnd;
  return 0;
}
static DDRESULT API clip_init(DD *c, DD *d, DWORD flags) {
  GUARD;
  (void)d;
  struct dd_clipper *p = c->owner;
  if (flags)
    return DDERR_INVALIDPARAMS;
  if (p->initialized)
    return DDERR(5);
  p->initialized = 1;
  return 0;
}
static DDRESULT API clip_changed(DD *c, BOOL *out) {
  GUARD;
  if (!out)
    return DDERR_INVALIDPARAMS;
  struct dd_clipper *p = c->owner;
  *out = p->changed || p->hwnd;
  p->changed = 0;
  return 0;
}
static DDRESULT API clip_set(DD *c, RGNDATA *in, DWORD flags) {
  GUARD;
  struct dd_clipper *p = c->owner;
  if (flags)
    return DDERR_INVALIDPARAMS;
  if (p->hwnd)
    return DDERR_CLIPPERISUSINGHWND;
  RECT *replacement = 0;
  DWORD n = 0;
  if (in) {
    n = in->rdh.nCount;
    if (in->rdh.dwSize != 32 || in->rdh.iType != 1 || n > DD_MAX_RECTS ||
        in->rdh.nRgnSize != n * sizeof(RECT))
      return DDERR_INVALIDPARAMS;
    replacement = dd_alloc(n * sizeof(RECT) + 1);
    if (!replacement)
      return DDERR_OUTOFMEMORY;
    w32_copy(replacement, in->Buffer, n * sizeof(RECT));
    for (DWORD i = 0; i < n; i++)
      if (replacement[i].left > replacement[i].right ||
          replacement[i].top > replacement[i].bottom) {
        dd_free(replacement);
        return DDERR_INVALIDRECT;
      }
  }
  dd_free(p->rects);
  p->rects = replacement;
  p->count = n;
  p->changed = 1;
  return 0;
}
static DDRESULT API clip_set_hwnd(DD *c, DWORD flags, uintptr_t hwnd) {
  GUARD;
  if (flags)
    return DDERR_INVALIDPARAMS;
  struct dd_clipper *p = c->owner;
  if (hwnd) {
    DWORD count = 0;
    DDRESULT r =
        dd_backend->clip ? dd_backend->clip(hwnd, 0, &count) : DDERR_NOCLIPLIST;
    if (r)
      return r;
  }
  p->hwnd = hwnd;
  p->changed = 1;
  return 0;
}
const void *const dd_clipper_vt[] = {dd_query,     dd_add,    dd_release,
                                     clip_list,    clip_hwnd, clip_init,
                                     clip_changed, clip_set,  clip_set_hwnd};
DDRESULT API DirectDrawCreateClipper(DWORD flags, DD **out, void *outer) {
  GUARD;
  if (!out)
    return DDERR_INVALIDPARAMS;
  *out = 0;
  if (flags)
    return DDERR_INVALIDPARAMS;
  if (outer)
    return (DDRESULT)0x80040110u;
  struct dd_clipper *p = dd_alloc(sizeof(*p));
  if (!p)
    return DDERR_OUTOFMEMORY;
  DWORD gen = 1;
  const void *const *vt = dd_clipper_vt;
  dd_init(&p->base, 4, sizeof(*p), &gen, &vt, 1);
  p->initialized = 1;
  *out = &p->base.interfaces[0];
  return 0;
}
DDRESULT dd_create_clipper(DD *c, DWORD flags, DD **out, void *outer) {
  (void)c;
  return DirectDrawCreateClipper(flags, out, outer);
}
