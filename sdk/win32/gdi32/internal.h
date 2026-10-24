/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef W32_GDI_INTERNAL_H
#define W32_GDI_INTERNAL_H
#include <w32/gdi32.h>
#include <w32/services.h>
#define GDI_LIMIT 32767
#define GDI_MAX_RECTS 32768u
enum {
  GDI_PEN = 1,
  GDI_BRUSH = 2,
  GDI_DC = 3,
  GDI_PALETTE = 5,
  GDI_FONT = 6,
  GDI_BITMAP = 7,
  GDI_REGION = 8,
  GDI_METAFILE = 9, /* f5-20B: OBJ_METAFILE */
  GDI_MEMDC = 10,
  GDI_ENHMETAFILE = 13 /* f5-20B: OBJ_ENHMETAFILE */
};
struct region {
  RECT *rects;
  unsigned count, capacity;
};
struct bitmap {
  struct w32_dib dib;
  void *allocation, *section;
  DWORD alloc_size, offset;
  HDC selected_dc;
  int mono, dib_section;
};
struct brush {
  LOGBRUSH log;
  struct w32_dib pattern;
  uint8_t pixels[256];
};
struct font {
  struct w32_gdi_font_provider provider;
  void *face;
  unsigned em;
};
struct palette {
  PALETTEENTRY entries[256];
  unsigned count;
  uint8_t mapping[256];
  DWORD realized_epoch;
  unsigned dirty;
};
struct dc_state {
  HGDIOBJ pen, brush, font, bitmap, palette;
  COLORREF bk, text, dcbrush, dcpen;
  int bk_mode, rop2, stretch, fill, map, extra, palette_background;
  unsigned align;
  POINT window_org, viewport_org, brush_org, current;
  SIZE window_ext, viewport_ext;
  struct region clip;
  int has_clip;
};
struct saved {
  struct saved *next;
  struct dc_state state;
  int level;
};
struct dc {
  struct dc_state s;
  struct saved *saved;
  int level, memory, pixel_format;
  int *window_pixel_format; /* USER owns this slot until the DC is released. */
  struct w32_dib surface;
  struct region visible, shape;
  int has_visible, has_shape;
  POINT origin, screen_origin;
  void (*damage)(void *, const RECT *);
  void *opaque;
  struct gdi_recorder *record; /* f5-20B: a metafile DC records its calls */
};
struct object {
  unsigned type, refs, stock;
  union {
    struct bitmap bitmap;
    struct brush brush;
    LOGPEN pen;
    struct font font;
    struct palette palette;
    struct region region;
    struct dc dc;
    struct { /* f5-20B: Windows (16-bit) and enhanced metafile bits */
      uint8_t *bits;
      DWORD size;
    } meta;
  } u;
};
void *gdi_alloc(DWORD);
void gdi_free(void *, DWORD);
void gdi_lock(void);
void gdi_unlock(int *);
#define GDI_GUARD                                                              \
  int gdi_guard __attribute__((cleanup(gdi_unlock))) = (gdi_lock(), 0)
struct object *gdi_object(HGDIOBJ, unsigned);
struct object *gdi_new(unsigned, HGDIOBJ *);
struct dc *gdi_dc(HDC);
struct w32_dib *gdi_target(struct dc *);
void gdi_ref(HGDIOBJ, int);
int gdi_region_copy(struct region *, const struct region *);
void gdi_region_free(struct region *);
int gdi_region_add(struct region *, RECT);
int gdi_region_combine(struct region *, const struct region *,
                       const struct region *, int);
int gdi_region_point(const struct region *, int, int);
RECT gdi_region_box(const struct region *);
int gdi_region_type(const struct region *);
int gdi_region_shift(struct region *, int, int);
int gdi_visible(void *, int, int);
int gdi_transform(struct dc *, POINT *, int, int);
RECT gdi_rect(struct dc *, RECT);
void gdi_damage(struct dc *, RECT);
DWORD gdi_color(COLORREF);
DWORD gdi_resolve_color(struct dc *, COLORREF);
DWORD gdi_encode_color(struct dc *, COLORREF);
DWORD gdi_device_bpp(struct dc *);
void gdi_brush_pattern(struct dc *, struct w32_dib *, void *);
COLORREF gdi_colorref(DWORD);
int gdi_fill(struct dc *, RECT, HBRUSH, int);
int gdi_brush_pixel(struct dc *, HBRUSH, int, int, DWORD *);
BOOL gdi_blt(struct dc *, int, int, int, int, const struct w32_dib *, int, int,
             int, int, DWORD, int64_t);
int gdi_info(HDC, const BITMAPINFO *, unsigned, void *, struct w32_dib *);
void gdi_system_palette(DWORD *);
void gdi_palette_targets(void);
void gdi_palette_reset(void);
int gdi_smoothing(void);
int gdi_text(HDC, int, int, unsigned, const RECT *, const void *, unsigned,
             const int *, int);
/* f5-10C selection layer (font.c). */
void gdi_font_stock(unsigned, struct font *);
void gdi_font_exit(void);
int gdi_font_validate(const void *, DWORD);
/* f5-10C2 PE-to-native font bridge (bridge.c): the guest's default backend. */
extern const struct w32_gdi_face_backend gdi_bridge_backend;
/* f5-20B. */
#if defined(_WIN32) && !defined(W32_HOST_MODEL)
__declspec(dllimport) void W32API SetLastError(DWORD);
#else
void W32API SetLastError(DWORD);
#endif
DWORD gdi_cp1252_unicode(unsigned);
int gdi_cp1252_ansi(DWORD);
/* Metafile recording (metafile.c): each hook returns nonzero when the DC
 * is a metafile DC and the call was recorded (its result in *result). */
struct gdi_recorder;
int gdi_record(struct dc *, unsigned function, const int16_t *params,
               unsigned count);
int gdi_record_select(HDC, struct dc *, HGDIOBJ);
int gdi_record_text(struct dc *, int, int, unsigned, const RECT *,
                    const void *, unsigned, const int *, int);
int gdi_record_poly(struct dc *, unsigned function, const POINT *,
                    const int *, unsigned);
int gdi_record_dib(struct dc *, int, int, int, int, int, int, int, int,
                   const void *, const BITMAPINFO *, unsigned, DWORD);
int gdi_record_bitmap(struct dc *, int, int, int, int, const struct w32_dib *,
                      int, int, int, int, DWORD);
void gdi_record_free(struct dc *);
#endif
