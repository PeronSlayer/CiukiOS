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
static struct {
  BOOL active, changing;
  DWORD width, height, bpp;
  HWND carrier;
} cds_mode;
static void release_window(struct u32_window *);
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
  /* Only a value snapshot is read by pacing workers. The drawable's cached
   * tuple is filled by the owner-thread lease call below. */
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
  struct u32_window *carrier = u32_window(cds_mode.carrier);
  if (cds_mode.active && carrier && carrier != w && carrier->lease.active) {
    if (width == cds_mode.width && height == cds_mode.height && bpp == cds_mode.bpp) {
      w->lease = carrier->lease;
      w->lease_carrier = carrier->handle;
      w->lease_failed = 0;
      *out = w->lease;
      u32_display_cache();
      return 0;
    }
    /* A render-resolution/depth shim may use another frame layout. Prove
     * that layout with its own grant instead of submitting the wrong size
     * to the preflight token. The recorded application mode stays intact. */
    release_window(carrier);
  }
  if (w->lease_carrier) release_window(w);
  if (w->lease_wanted && w->lease_w == width && w->lease_h == height && w->lease_bpp == bpp) {
    *out = w->lease; return 0;
  }
  if (w->lease_wanted) w32_user_display_release(hwnd);
  uint8_t b[16]; desk_put32(b, w->handle == cds_mode.carrier ? DESK_V3_LEASE_CARRIER : 1);
  desk_put32(b+4, width); desk_put32(b+8, height); desk_put32(b+12, bpp);
  w->lease_w = width; w->lease_h = height; w->lease_bpp = bpp; w->lease_wanted = 1;
  if (!u32_wire(w, DESK_V3_LEASE, b, sizeof(b), -1)) {
    w->lease_failed = GetLastError() == 31;
    w->lease_wanted = 0; w32_fill(&w->lease, 0, sizeof(w->lease)); u32_display_cache();
    return GetLastError() == 170 ? DD_EXCLUSIVE : DD_UNSUPPORTED;
  }
  w->lease_failed = 0;
  *out = w->lease; u32_display_cache(); return 0;
}
static void release_window(struct u32_window *w) {
  HWND borrowed = w->lease_carrier;
  w->lease_carrier = 0;
  struct u32_window *carrier = u32_window(borrowed);
  if (carrier && carrier != w) release_window(carrier);
  BOOL wanted = w->lease_wanted;
  struct dd_lease before = w->lease;
  if (wanted)
    for (UINT i = 0; i < U32_WINDOWS; i++)
      if (u32_windows[i].lease_carrier == w->handle ||
          (before.active && u32_windows[i].lease.token == before.token &&
           u32_windows[i].lease.generation == before.generation)) {
        w32_fill(&u32_windows[i].lease, 0, sizeof(w->lease));
        u32_windows[i].lease_carrier = 0;
      }
  if (w->destroying && w->handle == cds_mode.carrier) {
    cds_mode.active = 0;
    cds_mode.carrier = 0;
  }
  w->lease_wanted = 0; w32_fill(&w->lease, 0, sizeof(w->lease)); u32_display_cache();
  if (wanted && w->wire && !u32_exiting) { uint8_t b[16] = {0}; u32_wire(w, DESK_V3_LEASE, b, sizeof(b), -1); }
}
void W32API w32_user_display_release(uintptr_t hwnd) {
  U32_GUARD;
  struct u32_window *w = u32_window((HWND)hwnd);
  if (!w || w->parent || (!w->destroying && w->tid != u32_tid())) return;
  release_window(w);
}
void u32_display_event(struct u32_window *w, const struct desk_packet *p) {
  struct dd_lease before = w->lease;
  if (p->opcode == DESK_V3_ACTIVATE && !p->focused && !p->flags)
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
  /* A CDS carrier's aliases have no independent grant. Clear them before a
   * deactivation callback or a pacing worker can reuse a revoked token. */
  if (w->handle == cds_mode.carrier && before.active && !w->lease.active)
    for (UINT i = 0; i < U32_WINDOWS; i++)
      if (u32_windows[i].lease.token == before.token &&
          u32_windows[i].lease.generation == before.generation)
        w32_fill(&u32_windows[i].lease, 0, sizeof(w->lease));
  u32_display_cache();
}

/* f7 (Quake III Arena): ChangeDisplaySettings and EnumDisplaySettings.
 *
 * Windows 98 (ChangeDisplaySettingsA, EnumDisplaySettingsA, DEVMODEA in the
 * Platform SDK): CDS_FULLSCREEN makes a temporary mode change for the
 * program; ChangeDisplaySettings(NULL, 0) returns to the registry mode;
 * CDS_TEST only validates. On CiukiOS the desktop composes every window and
 * owns the physical mode; a Win32 program reaches the display only through
 * the full-screen lease (DESK_V3_LEASE). Acquire synchronously through an
 * offscreen tool window: CDS commonly precedes creation of the drawable.
 * The drawable reuses the lease so that success means the driver accepted
 * the mode, including when firmware is quarantined. Persisting a mode (CDS_UPDATEREGISTRY,
 * CDS_GLOBAL, CDS_SET_PRIMARY, CDS_NORESET) is the Display control panel's
 * job: DISP_CHANGE_NOTUPDATED. A mode is valid under the DirectDraw rule
 * (sdk/win32/ddraw/device.c mode_supported): 8, 16 or 32 bits per pixel and
 * either a hardware mode size or one of the logical sizes the presenter
 * scales, with a 16/32-bit hardware mode at least that large. */
#define DISP_SUCCESSFUL 0
#define DISP_FAILED (-1)
#define DISP_BADMODE (-2)
#define DISP_NOTUPDATED (-3)
#define DISP_BADFLAGS (-4)
#define DISP_BADPARAM (-5)
#define CDS_UPDATEREGISTRY_ 1u
#define CDS_TEST_ 2u
#define CDS_FULLSCREEN_ 4u
#define CDS_PERSIST_ (1u | 8u | 0x10u | 0x10000000u) /* registry, global, primary, noreset */
#define CDS_RESET_ 0x40000000u
#define DM_BITSPERPEL_ 0x40000u
#define DM_PELSWIDTH_ 0x80000u
#define DM_PELSHEIGHT_ 0x100000u
#define DM_DISPLAYFLAGS_ 0x200000u
#define DM_DISPLAYFREQUENCY_ 0x400000u
/* DEVMODEA offsets: dmSize, dmFields, dmBitsPerPel, dmPelsWidth,
 * dmPelsHeight, dmDisplayFlags, dmDisplayFrequency; the minimum dmSize that
 * holds them (the Windows 95 DEVMODE through dmDisplayFrequency). The
 * Unicode forms stay Windows 9x typed failures (scripts/win32/user32.py). */
struct devmode_layout { UINT size, fields, bpp, width, height, flags, freq, minimum; };
static const struct devmode_layout devmode_a = {36, 40, 104, 108, 112, 116, 120, 124};
static const DWORD logical_sizes[][2] = {{320, 200}, {320, 240}, {400, 300}, {512, 384},
                                         {640, 400}, {640, 480}, {800, 600}, {1024, 768}};
static BOOL display_query(struct ciuki_display_info *d) {
  w32_fill(d, 0, sizeof(*d));
  d->version = CIUKI_DISPLAY_VERSION;
  d->size = sizeof(*d);
  return (int32_t)u32_backend.gate(CIUKI_SYS_DISPLAY, CIUKI_DISPLAY_QUERY, U32_PTR(d), 0) == 0 &&
         d->width && d->height && d->mode_count <= 32;
}
static BOOL mode_valid(const struct ciuki_display_info *d, DWORD w, DWORD h, DWORD bpp) {
  if (!w || !h || (bpp != 8 && bpp != 16 && bpp != 32))
    return 0;
  BOOL listed = 0;
  for (UINT i = 0; i < 8; i++)
    if (logical_sizes[i][0] == w && logical_sizes[i][1] == h)
      listed = 1;
  for (UINT i = 0; i < d->mode_count; i++)
    if (d->modes[i].width == w && d->modes[i].height == h)
      listed = 1;
  if (!listed)
    return 0;
  for (UINT i = 0; i < d->mode_count; i++)
    if (d->modes[i].width >= w && d->modes[i].height >= h &&
        (d->modes[i].bpp == 16 || d->modes[i].bpp == 32))
      return 1;
  return 0;
}
/* The bpp a program sees for the composed desktop (its DIB surfaces). */
static DWORD desktop_bpp(const struct ciuki_display_info *d) {
  return d->bpp == 8 || d->bpp == 16 ? d->bpp : 32;
}
static void cds_release(void) {
  HWND carrier = cds_mode.carrier;
  cds_mode.active = 0;
  cds_mode.carrier = 0;
  if (carrier) {
    w32_user_display_release((uintptr_t)carrier);
    DestroyWindow(carrier);
  }
  /* Clear aliases of the carrier tuple as well as leases reacquired by the
   * real drawable after Alt-Tab. No stale exclusive snapshot survives CDS(0). */
  for (UINT i = 0; i < U32_WINDOWS; i++) {
    struct u32_window *w = &u32_windows[i];
    if (w->handle) {
      /* CDS resets the process mode, including a real drawable reacquired
       * by another GUI thread after Alt-Tab. The public release API keeps
       * its owner-thread rule; this internal reset runs under USER's lock. */
      if (w->lease_wanted) release_window(w);
      w32_fill(&w->lease, 0, sizeof(w->lease));
      w->lease_carrier = 0;
      w->lease_failed = 0;
    }
  }
  u32_display_cache();
}
static LONG change_settings(const uint8_t *dm, const struct devmode_layout *l, DWORD flags) {
  U32_GUARD;
  if (cds_mode.changing) return DISP_FAILED;
  struct u32_window *owner = u32_window(cds_mode.carrier);
  if (owner && owner->tid != u32_tid()) return DISP_FAILED;
  if (flags & ~(CDS_PERSIST_ | CDS_TEST_ | CDS_FULLSCREEN_ | CDS_RESET_))
    return DISP_BADFLAGS;
  if ((flags & CDS_FULLSCREEN_) && (flags & CDS_UPDATEREGISTRY_))
    return DISP_BADFLAGS;
  if (!dm) {
    if (!(flags & CDS_TEST_))
      cds_release();
    return DISP_SUCCESSFUL;
  }
  DWORD size = w32_u16(dm + l->size), fields = w32_u32(dm + l->fields);
  if (size < l->minimum)
    return DISP_BADPARAM;
  struct ciuki_display_info d;
  if (!display_query(&d))
    return DISP_BADMODE;
  DWORD w = fields & DM_PELSWIDTH_ ? w32_u32(dm + l->width) : d.width;
  DWORD h = fields & DM_PELSHEIGHT_ ? w32_u32(dm + l->height) : d.height;
  DWORD bpp = fields & DM_BITSPERPEL_ ? w32_u32(dm + l->bpp) : desktop_bpp(&d);
  if (bpp == 15)
    bpp = 16;
  /* dmDisplayFrequency is accepted and ignored: the presenter paces at the
   * mode's own rate, as Windows 98 drivers without refresh control did. */
  if (!mode_valid(&d, w, h, bpp))
    return DISP_BADMODE;
  if (flags & CDS_TEST_)
    return DISP_SUCCESSFUL;
  if (flags & CDS_PERSIST_)
    return DISP_NOTUPDATED;
  struct u32_window *old = u32_window(cds_mode.carrier);
  if (!(flags & CDS_RESET_) && cds_mode.active && old && old->lease.active && cds_mode.width == w &&
      cds_mode.height == h && cds_mode.bpp == bpp) return DISP_SUCCESSFUL;
  cds_mode.changing = 1;
  cds_release();
  HWND previous = GetForegroundWindow();
  HWND carrier = CreateWindowExA(WS_EX_TOOLWINDOW, "STATIC", "", WS_POPUP | WS_VISIBLE,
                                  -8192, -8192, 32, 32, 0, 0, 0, 0);
  cds_mode.carrier = carrier;
  struct dd_lease lease = {0};
  BOOL ok = carrier && SetForegroundWindow(carrier) &&
      !w32_user_display_lease((uintptr_t)carrier, w, h, bpp, &lease) && lease.active;
  if (!ok) {
    if (carrier) DestroyWindow(carrier);
    cds_mode.carrier = 0;
    if (previous && IsWindow(previous)) SetForegroundWindow(previous);
    cds_mode.changing = 0;
    return DISP_FAILED;
  }
  cds_mode.carrier = carrier;
  cds_mode.active = 1;
  cds_mode.width = w;
  cds_mode.height = h;
  cds_mode.bpp = bpp;
  cds_mode.changing = 0;
  return DISP_SUCCESSFUL;
}
LONG W32API ChangeDisplaySettingsA(void *dm, DWORD flags) {
  return change_settings(dm, &devmode_a, flags);
}
/* Value-only display query for gdi32 GetDeviceCaps (HORZRES, VERTRES,
 * BITSPIXEL of display DCs): width, height and bits per pixel of the
 * physical mode as programs see the composed desktop (8 or 16 as is, 24
 * and 32 as 32, the depth of every window surface). */
BOOL W32API w32_user_display_caps(DWORD *out) {
  struct ciuki_display_info d;
  if (!out || !display_query(&d))
    return 0;
  out[0] = d.width;
  out[1] = d.height;
  out[2] = desktop_bpp(&d);
  return 1;
}
/* Value-only query for gdi32 (device gamma ramps): the full-screen mode this
 * process set with ChangeDisplaySettings, if any (width, height, bpp). */
BOOL W32API w32_user_display_mode(DWORD *out) {
  U32_GUARD;
  if (!cds_mode.active)
    return 0;
  if (out) {
    out[0] = cds_mode.width;
    out[1] = cds_mode.height;
    out[2] = cds_mode.bpp;
  }
  return 1;
}
/* ENUM_CURRENT_SETTINGS (-1) is the program's full-screen mode while one is
 * set, else the physical mode; ENUM_REGISTRY_SETTINGS (-2) the desktop's
 * physical mode; 0, 1, ... the modes ChangeDisplaySettings accepts, by
 * bits per pixel, then width, then height, each reported at 60 Hz. */
static BOOL enum_settings(const char *device, DWORD index, uint8_t *dm, const struct devmode_layout *l) {
  U32_GUARD;
  if (device && *device && w32_stricmp(device, "DISPLAY") && w32_stricmp(device, "\\\\.\\DISPLAY1"))
    return u32_fail(87);
  if (!dm || w32_u16(dm + l->size) < l->minimum)
    return u32_fail(87);
  struct ciuki_display_info d;
  if (!display_query(&d))
    return u32_fail(31);
  DWORD w = 0, h = 0, bpp = 0;
  if (index == 0xffffffffu && cds_mode.active) {
    w = cds_mode.width; h = cds_mode.height; bpp = cds_mode.bpp;
  } else if (index == 0xffffffffu || index == 0xfffffffeu) {
    w = d.width; h = d.height; bpp = desktop_bpp(&d);
  } else {
    DWORD sizes[8 + 32][2], count = 0;
    for (UINT i = 0; i < 8 + d.mode_count; i++) {
      DWORD sw = i < 8 ? logical_sizes[i][0] : d.modes[i - 8].width;
      DWORD sh = i < 8 ? logical_sizes[i][1] : d.modes[i - 8].height;
      BOOL seen = 0;
      for (DWORD k = 0; k < count; k++)
        if (sizes[k][0] == sw && sizes[k][1] == sh)
          seen = 1;
      if (seen)
        continue;
      DWORD at = count++;
      while (at && (sizes[at - 1][0] > sw || (sizes[at - 1][0] == sw && sizes[at - 1][1] > sh))) {
        sizes[at][0] = sizes[at - 1][0];
        sizes[at][1] = sizes[at - 1][1];
        at--;
      }
      sizes[at][0] = sw;
      sizes[at][1] = sh;
    }
    DWORD n = 0;
    for (DWORD b = 8; b <= 32 && !bpp; b *= 2)
      for (DWORD k = 0; k < count && !bpp; k++)
        if (mode_valid(&d, sizes[k][0], sizes[k][1], b) && n++ == index) {
          w = sizes[k][0]; h = sizes[k][1]; bpp = b;
        }
    if (!bpp)
      return u32_fail(18); /* ERROR_NO_MORE_FILES: past the last mode */
  }
  w32_put32(dm + l->fields, DM_BITSPERPEL_ | DM_PELSWIDTH_ | DM_PELSHEIGHT_ |
                                DM_DISPLAYFLAGS_ | DM_DISPLAYFREQUENCY_);
  w32_put32(dm + l->bpp, bpp);
  w32_put32(dm + l->width, w);
  w32_put32(dm + l->height, h);
  w32_put32(dm + l->flags, 0);
  w32_put32(dm + l->freq, 60);
  return 1;
}
BOOL W32API EnumDisplaySettingsA(const char *device, DWORD index, void *dm) {
  return enum_settings(device, index, dm, &devmode_a);
}
