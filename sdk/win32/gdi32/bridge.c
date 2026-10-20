/* f5-10C2 PE-to-native font bridge, client side: the default face backend
 * of the guest personality. Faces are opened and rasterised once, system-
 * wide, by w32srv (apps/w32srv/fonts.c, libfont over FreeType 2.13.3 v35);
 * the selection layer (font.c) keeps the processed coverage in its per-
 * process LRU. Requests travel over the serialized w32srv transport
 * (w32_server_exchange, 256-byte stop-and-wait fragments); no font file is
 * mapped into the Win32 process by this path. Wire layout: w32/server.h.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
#include <w32/server.h>

#define PAGES 4u
#define SMALL_REPLY 4096u
#define LARGE_REPLY (W32R_DATA + 64u)
struct page {
  DWORD base, stamp;
  int valid;
  uint16_t ids[256];
};
struct bridge_face {
  char path[260];
  DWORD path_bytes, clock;
  struct page pages[PAGES];
};
static w32_gdi_bridge_exchange transport;
static struct w32_gdi_bridge_stats stats;

#ifdef W32_HOST_MODEL
void w32_gdi_bridge_transport(w32_gdi_bridge_exchange fn) { transport = fn; }
#endif
static w32_gdi_bridge_exchange channel(void) {
#ifndef W32_HOST_MODEL
  if (!transport)
    transport = w32_server_exchange;
#endif
  return transport;
}
/* One transaction; returns the Win32 error (transport or service). */
static DWORD call(const struct bridge_face *f, uint32_t op, uint32_t index,
                  uint32_t count, const uint32_t *glyphs, uint8_t *reply,
                  DWORD cap, struct w32r_reply *r, DWORD *used) {
  w32_gdi_bridge_exchange fn = channel();
  if (!fn)
    return 120;
  uint8_t request[sizeof(struct w32r_request) + 260 + 1 + 4 * 4];
  DWORD path = f ? f->path_bytes : 1, data = 4 * count;
  if (data > 16)
    return 87;
  struct w32r_request a = {W32R_VERSION, op, 0, index, count, 0, 0, path, 1,
                           data};
  w32_copy(request, &a, sizeof(a));
  if (f)
    w32_copy(request + sizeof(a), f->path, path);
  else
    request[sizeof(a)] = 0;
  request[sizeof(a) + path] = 0;
  if (data)
    w32_copy(request + sizeof(a) + path + 1, glyphs, data);
  stats.requests++;
  uint32_t n = 0;
  uint32_t e = fn(request, sizeof(a) + path + 1 + data, reply, cap, &n);
  if (!e && n < sizeof(*r))
    e = 13;
  if (!e) {
    w32_copy(r, reply, sizeof(*r));
    if (r->version != W32R_VERSION || r->data_bytes > n - sizeof(*r))
      e = 13;
    else
      e = r->error;
  }
  *used = n;
  return e;
}
static void failed(DWORD e) {
  stats.last_error = e;
  if (e == 13 || e == 50 || e == 8 || e == 5 || e == 2)
    stats.refusals++;
  else
    stats.errors++;
}
static int load_page(struct bridge_face *f, struct page *p, DWORD base) {
  uint8_t reply[sizeof(struct w32r_reply) + 512];
  struct w32r_reply r;
  DWORD used;
  DWORD e = call(f, W32R_FONT_CMAP, base, 0, 0, reply, sizeof(reply), &r, &used);
  if (!e && r.data_bytes != 512)
    e = 13;
  if (e) {
    failed(e);
    return 0;
  }
  for (unsigned i = 0; i < 256; i++)
    p->ids[i] = (uint16_t)(reply[sizeof(r) + 2 * i] |
                           reply[sizeof(r) + 2 * i + 1] << 8);
  p->base = base;
  p->valid = 1;
  p->stamp = ++f->clock;
  stats.cmap_pages++;
  return 1;
}
static void *bridge_open(const char *path) {
  DWORD n = path ? w32_strlen(path) : 0;
  if (!n || n >= sizeof(((struct bridge_face *)0)->path))
    return 0;
  struct bridge_face *f = gdi_alloc(sizeof(*f));
  if (!f)
    return 0;
  w32_copy(f->path, path, n + 1);
  f->path_bytes = n + 1;
  /* Code points 0-255 first: opens the face in the service and reports a
   * refusal (W21) before the selection layer relies on it. */
  if (!load_page(f, &f->pages[0], 0)) {
    gdi_free(f, sizeof(*f));
    return 0;
  }
  stats.open_faces++;
  return f;
}
static void bridge_close(void *o) {
  if (o) {
    stats.open_faces--;
    gdi_free(o, sizeof(struct bridge_face));
  }
}
static int bridge_lookup(void *o, DWORD cp, unsigned *glyph) {
  struct bridge_face *f = o;
  if (cp >= 0x110000u)
    return 0;
  DWORD base = cp & ~255u;
  struct page *p = 0, *lru = &f->pages[0];
  for (unsigned i = 0; i < PAGES && !p; i++) {
    if (f->pages[i].valid && f->pages[i].base == base)
      p = &f->pages[i];
    else if (!f->pages[i].valid || (lru->valid && f->pages[i].stamp < lru->stamp))
      lru = &f->pages[i];
  }
  if (!p && !load_page(f, p = lru, base))
    return 0;
  p->stamp = ++f->clock;
  *glyph = p->ids[cp & 255];
  return *glyph != 0;
}
static int bridge_raster(void *o, unsigned glyph, unsigned em26,
                         struct w32_gdi_glyph *out) {
  struct bridge_face *f = o;
  uint8_t small[SMALL_REPLY], *reply = small;
  DWORD cap = sizeof(small), used = 0;
  struct w32r_reply r;
  uint32_t id = glyph;
  DWORD e = call(f, W32R_FONT_GLYPHS, em26, 1, &id, reply, cap, &r, &used);
  if (e == 13 && used == 0) {
    /* The reply exceeded the stack buffer: large glyphs only. */
    reply = gdi_alloc(LARGE_REPLY);
    if (!reply) {
      failed(8);
      return 0;
    }
    cap = LARGE_REPLY;
    e = call(f, W32R_FONT_GLYPHS, em26, 1, &id, reply, cap, &r, &used);
  }
  int ok = 0;
  struct w32r_glyph g;
  if (!e && (r.count != 1 || r.data_bytes < sizeof(g)))
    e = 13;
  if (!e) {
    w32_copy(&g, reply + sizeof(r), sizeof(g));
    DWORD bytes = (DWORD)g.width * g.height;
    if (g.glyph != glyph || r.data_bytes != sizeof(g) + bytes)
      e = 13;
    else {
      uint8_t *coverage = bytes ? gdi_alloc(bytes) : 0;
      if (bytes && !coverage)
        e = 8;
      else {
        if (bytes)
          w32_copy(coverage, reply + sizeof(r) + sizeof(g), bytes);
        *out = (struct w32_gdi_glyph){g.width, g.height, g.width, g.left,
                                      g.top,   g.advance, coverage};
        stats.glyphs++;
        ok = 1;
      }
    }
  }
  if (e)
    failed(e);
  if (reply != small)
    gdi_free(reply, LARGE_REPLY);
  return ok;
}
static void bridge_free(void *o, struct w32_gdi_glyph *g) {
  (void)o;
  if (g->coverage)
    gdi_free(g->coverage, g->pitch * g->height);
  g->coverage = 0;
}
/* The face itself lives once in w32srv, accounted against the shared
 * service budget (w32r_font_stats); this process holds only the client. */
static DWORD bridge_resident(void *o) {
  (void)o;
  return (DWORD)sizeof(struct bridge_face);
}
const struct w32_gdi_face_backend gdi_bridge_backend = {
    bridge_open, bridge_close, bridge_lookup, bridge_raster, bridge_free,
    bridge_resident};
const struct w32_gdi_face_backend *W32API w32_gdi_bridge_backend(void) {
  return &gdi_bridge_backend;
}
int32_t W32API w32_gdi_bridge_statistics(struct w32_gdi_bridge_stats *out) {
  GDI_GUARD;
  if (!out)
    return 0;
  uint8_t reply[sizeof(struct w32r_reply) + sizeof(struct w32r_font_stats)];
  struct w32r_reply r;
  struct w32r_font_stats s = {0};
  DWORD used, e = call(0, W32R_FONT_STATS, 0, 0, 0, reply, sizeof(reply), &r,
                       &used);
  if (!e && r.data_bytes == sizeof(s))
    w32_copy(&s, reply + sizeof(r), sizeof(s));
  *out = stats;
  out->service_open_faces = s.open_faces;
  out->service_face_bytes = s.face_bytes;
  out->service_face_peak = s.face_peak;
  out->service_budget = s.budget;
  out->service_glyphs = s.glyphs;
  out->service_opens = s.opens;
  out->service_evictions = s.evictions;
  out->service_refusals = s.refusals;
  return !e && r.data_bytes == sizeof(s);
}
