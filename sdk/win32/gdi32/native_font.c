/* Consume the pinned SDK backend without changing it.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "native_font.h"
#include "internal.h"
#include <ciuki/font.h>
struct native_face {
  struct ciuki_font_face *face;
  unsigned refs;
};
static void retain(void *opaque) { ((struct native_face *)opaque)->refs++; }
static void release(void *opaque) {
  struct native_face *f = opaque;
  if (--f->refs)
    return;
  ciuki_font_close(f->face);
  gdi_free(f, sizeof(*f));
}
static int metrics(void *opaque, unsigned em,
                   struct w32_gdi_font_metrics *out) {
  struct native_face *f = opaque;
  struct ciuki_font_size_metrics m;
  if (ciuki_font_get_size_metrics(f->face, em, &m))
    return 0;
  *out = (struct w32_gdi_font_metrics){
      m.ascent, m.descent < 0 ? -m.descent : m.descent,
      m.line_gap < 0 ? 0 : m.line_gap, m.max_advance};
  return 1;
}
static int raster(void *opaque, unsigned em, DWORD cp, int index,
                  struct w32_gdi_glyph *out) {
  struct native_face *f = opaque;
  unsigned glyph_index = cp;
  struct ciuki_font_bitmap bitmap;
  if (!index && ciuki_font_lookup(f->face, cp, &glyph_index))
    return 0;
  if (ciuki_font_raster(f->face, glyph_index, em, &bitmap))
    return 0;
  /* f5-10C2: libfont reports the top row as a y-down offset from the
   * baseline (-FT_GlyphSlot.bitmap_top); w32_gdi_glyph.top is the Win32
   * distance above the baseline (GLYPHMETRICS.gmptGlyphOrigin.y, FreeType
   * bitmap_top), which text.c composes as baseline - top. */
  *out = (struct w32_gdi_glyph){bitmap.width,   bitmap.height, bitmap.stride,
                                bitmap.left,    -bitmap.top,   bitmap.advance,
                                bitmap.coverage};
  return 1;
}
static void free_glyph(void *opaque, struct w32_gdi_glyph *g) {
  (void)opaque;
  struct ciuki_font_bitmap bitmap = {g->width, g->height,  g->pitch,   g->left,
                                     -g->top,  g->advance, g->coverage};
  ciuki_font_bitmap_free(&bitmap);
  g->coverage = 0;
}
static unsigned utf8(DWORD cp, char *out) {
  if (cp < 128) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 2048) {
    out[0] = (char)(0xc0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 63));
    return 2;
  }
  if (cp < 65536) {
    out[0] = (char)(0xe0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 63));
    out[2] = (char)(0x80 | (cp & 63));
    return 3;
  }
  out[0] = (char)(0xf0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 63));
  out[2] = (char)(0x80 | ((cp >> 6) & 63));
  out[3] = (char)(0x80 | (cp & 63));
  return 4;
}
static int kerning(void *opaque, unsigned em, DWORD previous, DWORD current,
                   int *delta) {
  struct native_face *f = opaque;
  char text[8];
  unsigned a = utf8(previous, text), b = utf8(current, text + a);
  int32_t pair, one, two;
  if (ciuki_font_measure(f->face, em, text, a + b, &pair) ||
      ciuki_font_measure(f->face, em, text, a, &one) ||
      ciuki_font_measure(f->face, em, text + a, b, &two))
    return 0;
  *delta = pair - one - two;
  return 1;
}
HFONT w32_gdi_native_font_open(const char *path, unsigned em) {
  struct native_face *f = gdi_alloc(sizeof(*f));
  if (!f)
    return 0;
  if (ciuki_font_open(path, &f->face)) {
    gdi_free(f, sizeof(*f));
    return 0;
  }
  f->refs = 1;
  const struct w32_gdi_font_provider provider = {.metrics = metrics,
                                                 .raster = raster,
                                                 .free_glyph = free_glyph,
                                                 .retain = retain,
                                                 .release = release,
                                                 .kerning = kerning};
  HFONT h = w32_gdi_create_font(&provider, f, em);
  release(f);
  return h;
}

/* f5-10C selection-layer backend. The SDK face keeps the file bytes and its
 * own 128-glyph cache; the selection layer caches processed glyphs, so the
 * SDK cache is trimmed to 64 KiB to keep the shared budget honest. */
#include <stdio.h>
struct store_face {
  struct ciuki_font_face *face;
  DWORD length;
};
static void *store_open(const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file)
    return 0;
  long length = fseek(file, 0, SEEK_END) ? -1 : ftell(file);
  fclose(file);
  struct store_face *f = gdi_alloc(sizeof(*f));
  if (!f || length <= 0)
    goto fail;
  f->length = (DWORD)length;
  if (ciuki_font_open(path, &f->face))
    goto fail;
  return f;
fail:
  if (f)
    gdi_free(f, sizeof(*f));
  return 0;
}
static void store_close(void *o) {
  struct store_face *f = o;
  ciuki_font_close(f->face);
  gdi_free(f, sizeof(*f));
}
static int store_lookup(void *o, DWORD cp, unsigned *glyph) {
  return !ciuki_font_lookup(((struct store_face *)o)->face, cp, glyph) &&
         *glyph;
}
static int store_raster(void *o, unsigned glyph, unsigned em26,
                        struct w32_gdi_glyph *out) {
  struct store_face *f = o;
  struct ciuki_font_bitmap b;
  if (ciuki_font_raster(f->face, glyph, em26, &b))
    return 0;
  /* libfont reports the top row as a y-down offset from the baseline;
   * w32_gdi_glyph.top is the distance above the baseline (text.c compose). */
  *out = (struct w32_gdi_glyph){b.width, b.height, b.stride, b.left,
                                -b.top,  b.advance, b.coverage};
  struct ciuki_font_cache_stats stats;
  if (!ciuki_font_cache_info(f->face, &stats) && stats.bytes > (64u << 10))
    ciuki_font_cache_clear(f->face);
  return 1;
}
static DWORD store_resident(void *o) {
  struct store_face *f = o;
  struct ciuki_font_cache_stats stats = {0};
  ciuki_font_cache_info(f->face, &stats);
  return f->length + (64u << 10) + (DWORD)stats.bytes;
}
const struct w32_gdi_face_backend *w32_gdi_native_backend(void) {
  static const struct w32_gdi_face_backend backend = {
      store_open, store_close, store_lookup, store_raster, free_glyph,
      store_resident};
  return &backend;
}
