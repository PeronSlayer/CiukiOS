/* Owner-thread v3 lease control; workers read value-only snapshots.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
#define DD_INVALID ((int32_t)0x80070057u)
#define DD_NOCLIP ((int32_t)0x887602cdu)
#define DD_MORE ((int32_t)0x887602b2u)
#define DD_EXCLUSIVE ((int32_t)0x887600e1u)
#define DD_UNSUPPORTED ((int32_t)0x80004001u)
/* Atomic scalars avoid C data races while a GUI writer replaces a snapshot.
 * A reader gives up after bounded retries, never waits for the GUI thread. */
struct cached {
  uintptr_t handle;
  DWORD seq, lease[7], count, rects[U32_RECTS * 4];
};
static struct cached cached[U32_WINDOWS];
static BOOL subtract(struct u32_region *r, RECT cut) {
  struct u32_region out = {0};
  for (UINT i = 0; i < r->n; i++) {
    RECT a = r->r[i], x;
    if (!IntersectRect(&x, &a, &cut)) {
      if (out.n == U32_RECTS) return 0;
      out.r[out.n++] = a;
      continue;
    }
    RECT pieces[4] = {{a.left,a.top,a.right,x.top},{a.left,x.bottom,a.right,a.bottom},
                     {a.left,x.top,x.left,x.bottom},{x.right,x.top,a.right,x.bottom}};
    for (UINT k = 0; k < 4; k++) if (!IsRectEmpty(&pieces[k])) {
      if (out.n == U32_RECTS) return 0;
      out.r[out.n++] = pieces[k];
    }
  }
  *r = out; return 1;
}
static BOOL client_screen(struct u32_window *w, RECT *out) {
  RECT r = {0, 0, w->rect.right - w->rect.left, w->rect.bottom - w->rect.top};
  int bx, by;
  u32_frame(w->style, w->exstyle, &bx, &by);
  r.right -= 2 * bx;
  r.bottom -= by + bx;
  if (r.right < 0) r.right = 0;
  if (r.bottom < 0) r.bottom = 0;
  POINT o = u32_origin(w);
  OffsetRect(&r, o.x, o.y);
  *out = r;
  return !IsRectEmpty(&r);
}
/* Screen-coordinate visible client area of W: its client rectangle clipped
 * by every ancestor's client area and the screen, minus this process's
 * visible top-level windows above its root. Other processes' windows are
 * known only to the desktop compositor, which clips the published surface. */
static void visible_area(struct u32_window *w, struct u32_region *visible) {
  visible->n = 0;
  struct u32_window *root = u32_root(w);
  if (!w->handle || w->destroying || !root || root->destroying ||
      !IsWindowVisible(w->handle) || IsIconic(root->handle))
    return;
  RECT r, a, screen = {0, 0, u32_screen_w, u32_screen_h};
  if (!client_screen(w, &r) || !IntersectRect(&r, &r, &screen))
    return;
  UINT depth = 0;
  for (struct u32_window *p = u32_window(w->parent); p && depth < U32_WINDOWS;
       p = u32_window(p->parent), depth++)
    if (!client_screen(p, &a) || !IntersectRect(&r, &r, &a))
      return;
  visible->r[0] = r;
  visible->n = 1;
  BOOL ours = !!(root->exstyle & WS_EX_TOPMOST);
  for (UINT j = 0; j < U32_WINDOWS; j++) {
    struct u32_window *q = &u32_windows[j];
    if (q == root || !q->handle || q->parent || q->destroying ||
        !IsWindowVisible(q->handle) || IsIconic(q->handle))
      continue;
    BOOL top = !!(q->exstyle & WS_EX_TOPMOST);
    if (top < ours || (top == ours && q->z <= root->z))
      continue;
    if (!subtract(visible, q->rect)) {
      visible->n = 0;
      return;
    }
  }
}
void u32_display_cache(void) {
  for (UINT i = 0; i < U32_WINDOWS; i++) {
    struct u32_window *w = &u32_windows[i];
    struct cached *c = &cached[i];
    struct u32_region visible = {0};
    visible_area(w, &visible);
    DWORD values[7];
    w32_copy(values, &w->lease, sizeof(values));
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&c->handle, (uintptr_t)w->handle, __ATOMIC_SEQ_CST);
    for (UINT k = 0; k < 7; k++)
      __atomic_store_n(&c->lease[k], w->parent ? 0 : values[k], __ATOMIC_SEQ_CST);
    __atomic_store_n(&c->count, visible.n, __ATOMIC_SEQ_CST);
    for (UINT k = 0; k < visible.n * 4; k++) {
      DWORD word;
      w32_copy(&word, (uint8_t *)visible.r + k * 4, 4);
      __atomic_store_n(&c->rects[k], word, __ATOMIC_SEQ_CST);
    }
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_SEQ_CST);
  }
}
/* 1 copied, 0 no such window, -1 a writer kept the slot busy. */
static int snapshot(uintptr_t hwnd, struct dd_lease *lease, RECT *r,
                    DWORD *count) {
  for (UINT i = 0; hwnd && i < U32_WINDOWS; i++) {
    struct cached *c = &cached[i];
    if (__atomic_load_n(&c->handle, __ATOMIC_SEQ_CST) != hwnd)
      continue;
    for (UINT tries = 0; tries < 256; tries++) {
      DWORD seq = __atomic_load_n(&c->seq, __ATOMIC_SEQ_CST);
      if (seq & 1)
        continue;
      DWORD words[7], n = __atomic_load_n(&c->count, __ATOMIC_SEQ_CST);
      if (n > U32_RECTS)
        continue;
      for (UINT k = 0; k < 7; k++)
        words[k] = __atomic_load_n(&c->lease[k], __ATOMIC_SEQ_CST);
      if (r)
        for (UINT k = 0; k < n * 4; k++) {
          DWORD word = __atomic_load_n(&c->rects[k], __ATOMIC_SEQ_CST);
          w32_copy((uint8_t *)r + k * 4, &word, 4);
        }
      if (seq != __atomic_load_n(&c->seq, __ATOMIC_SEQ_CST) ||
          __atomic_load_n(&c->handle, __ATOMIC_SEQ_CST) != hwnd)
        continue;
      if (lease)
        w32_copy(lease, words, sizeof(words));
      if (count)
        *count = n;
      return 1;
    }
    return -1;
  }
  return 0;
}
/* A busy slot leaves the caller's previous tuple untouched, so a pacing
 * worker never mistakes a concurrent GUI update for a revocation. */
void W32API w32_user_display_poll(uintptr_t hwnd, struct dd_lease *out) {
  if (out && !snapshot(hwnd, out, 0, 0))
    w32_fill(out, 0, sizeof(*out));
}
int32_t W32API w32_user_display_clip(uintptr_t hwnd, RECT *out, DWORD *count) {
  if (!count)
    return DD_INVALID;
  RECT r[U32_RECTS];
  DWORD n = 0;
  if (snapshot(hwnd, 0, r, &n) != 1) {
    *count = 0;
    return DD_NOCLIP;
  }
  DWORD cap = *count;
  *count = n;
  if (!out)
    return 0;
  if (cap < n)
    return DD_MORE;
  w32_copy(out, r, n * sizeof(*r));
  return 0;
}
int32_t W32API w32_user_display_lease(uintptr_t hwnd, DWORD width, DWORD height,
                                    DWORD bpp, struct dd_lease *out) {
  U32_GUARD;
  if (out) w32_fill(out, 0, sizeof(*out));
  struct u32_window *w = u32_window((HWND)hwnd);
  if (!w || w->parent || w->destroying || w->tid != u32_tid() || !out ||
      !width || !height || width > 2048 || height > 2048 || (bpp != 8 && bpp != 16 && bpp != 32)) return DD_INVALID;
  if (w->lease_wanted && w->lease_w == width && w->lease_h == height && w->lease_bpp == bpp) {
    *out = w->lease; return 0;
  }
  if (w->lease_wanted) w32_user_display_release(hwnd);
  uint8_t b[16]; desk_put32(b, 1); desk_put32(b+4, width); desk_put32(b+8, height); desk_put32(b+12, bpp);
  w->lease_w = width; w->lease_h = height; w->lease_bpp = bpp; w->lease_wanted = 1;
  if (!u32_wire(w, DESK_V3_LEASE, b, sizeof(b), -1)) {
    w->lease_wanted = 0; w32_fill(&w->lease, 0, sizeof(w->lease)); u32_display_cache();
    return GetLastError() == 170 ? DD_EXCLUSIVE : DD_UNSUPPORTED;
  }
  *out = w->lease; u32_display_cache(); return 0;
}
void W32API w32_user_display_release(uintptr_t hwnd) {
  U32_GUARD;
  struct u32_window *w = u32_window((HWND)hwnd);
  if (!w || w->parent || (!w->destroying && w->tid != u32_tid())) return;
  BOOL wanted = w->lease_wanted;
  w->lease_wanted = 0; w32_fill(&w->lease, 0, sizeof(w->lease)); u32_display_cache();
  if (wanted && w->wire) { uint8_t b[16] = {0}; u32_wire(w, DESK_V3_LEASE, b, sizeof(b), -1); }
}
void u32_display_event(struct u32_window *w, const struct desk_packet *p) {
  if (p->opcode == DESK_V3_ACTIVATE && !p->focused)
    w32_fill(&w->lease, 0, sizeof(w->lease));
  if (p->opcode == DESK_V3_LEASE_EVENT) {
    w32_fill(&w->lease, 0, sizeof(w->lease));
    /* A failed grant also drops the desktop's retained mode (no regrant). */
    if (p->flags == 3 && p->result < 0)
      w->lease_wanted = 0;
    if (w->lease_wanted && p->flags == 1 && p->lease.token && p->lease.generation &&
        p->width == w->lease_w && p->height == w->lease_h && p->bpp == w->lease_bpp &&
        p->lease.surface.width == p->width && p->lease.surface.height == p->height)
      w->lease = (struct dd_lease){1, p->lease.token, p->lease.generation,
          p->width, p->height, p->bpp, p->lease.surface.pitch};
  }
  u32_display_cache();
}
