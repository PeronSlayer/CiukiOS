/* Generational process-local objects, recursive serialization and DC state.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
#include "../../../apps/desktop/appearance.h"
#if defined(_WIN32) && !defined(W32_HOST_MODEL)
#define G23_IMPORT __declspec(dllimport)
#else
#define G23_IMPORT extern
#endif
G23_IMPORT void *W32API CreateFileA(const char *,DWORD,DWORD,void *,DWORD,DWORD,void *);
G23_IMPORT BOOL W32API ReadFile(void *,void *,DWORD,DWORD *,void *);
G23_IMPORT BOOL W32API CloseHandle(void *);
G23_IMPORT DWORD W32API GetTickCount(void);
#define SLOTS 4095u
static struct {
  struct object *object;
  DWORD generation;
} slots[SLOTS];
static struct object stocks[21];
static uint8_t mono_pixel[2];
static int initialized;
static volatile DWORD lock_owner;
static unsigned lock_depth;
static struct w32_dib desktop;
void *gdi_alloc(DWORD n) {
  const struct w32_services *s = w32_get_services();
  void *p = n ? s->alloc(s->opaque, n) : 0;
  if (p)
    w32_fill(p, 0, n);
  return p;
}
void gdi_free(void *p, DWORD n) {
  if (p) {
    const struct w32_services *s = w32_get_services();
    s->free(s->opaque, p, n);
  }
}
void gdi_lock(void) {
  const struct w32_services *s = w32_get_services();
  DWORD tid = s->thread_id ? s->thread_id(s->opaque) : 1;
  if (!tid)
    tid = 1;
  if (__atomic_load_n(&lock_owner, __ATOMIC_ACQUIRE) == tid) {
    lock_depth++;
    return;
  }
  for (;;) {
    DWORD owner = __sync_val_compare_and_swap(&lock_owner, 0, tid);
    if (!owner)
      break;
    if (s->wait_word)
      s->wait_word(s->opaque, &lock_owner, owner);
  }
  lock_depth = 1;
}
void gdi_unlock(int *unused) {
  (void)unused;
  if (--lock_depth)
    return;
  __atomic_store_n(&lock_owner, 0, __ATOMIC_RELEASE);
  const struct w32_services *s = w32_get_services();
  if (s->wake_word)
    s->wake_word(s->opaque, &lock_owner);
}
static void init(void) {
  if (initialized)
    return;
  initialized = 1;
  for (unsigned i = 0; i < 21; i++) {
    slots[i].object = &stocks[i];
    slots[i].generation = 1;
    stocks[i].stock = 1;
  }
  DWORD gray[] = {0xffffff, 0xc0c0c0, 0x808080, 0x404040, 0};
  for (unsigned i = 0; i < 6; i++) {
    stocks[i].type = GDI_BRUSH;
    stocks[i].u.brush.log = (LOGBRUSH){i == 5 ? BS_NULL : BS_SOLID,
                                       i < 5 ? gdi_colorref(gray[i]) : 0, 0};
  }
  for (unsigned i = 6; i <= 8; i++) {
    stocks[i].type = GDI_PEN;
    stocks[i].u.pen = (LOGPEN){
        i == 8 ? PS_NULL : PS_SOLID, {1, 0}, i == 6 ? RGB(255, 255, 255) : 0};
  }
  static const unsigned fonts[] = {10, 11, 12, 13, 14, 16, 17};
  for (unsigned i = 0; i < sizeof(fonts) / sizeof(*fonts); i++) {
    stocks[fonts[i]].type = GDI_FONT;
    gdi_font_stock(fonts[i], &stocks[fonts[i]].u.font);
  }
  stocks[15].type = GDI_PALETTE;
  DWORD system[256];
  gdi_system_palette(system);
  stocks[15].u.palette.count = 20;
  for (unsigned i = 0; i < 20; i++) {
    DWORD c = system[i < 10 ? i : 246 + i - 10];
    stocks[15].u.palette.entries[i] =
        (PALETTEENTRY){(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c, 0};
  }
  stocks[18].type = GDI_BRUSH;
  stocks[18].u.brush.log = (LOGBRUSH){BS_SOLID, RGB(255, 255, 255), 0};
  stocks[19].type = GDI_PEN;
  stocks[19].u.pen = (LOGPEN){PS_SOLID, {1, 0}, 0};
  stocks[20].type = GDI_BITMAP;
  stocks[20].u.bitmap.dib = (struct w32_dib){.bits = mono_pixel,
                                             .width = 1,
                                             .height = 1,
                                             .pitch = 2,
                                             .size = 2,
                                             .bpp = 1,
                                             .top_down = 1,
                                             .colors = {0, 0xffffff},
                                             .color_count = 2};
  stocks[20].u.bitmap.mono = 1;
}
static HGDIOBJ stock_handle(unsigned i) { return (1u << 12) | (i + 1); }
struct object *gdi_object(HGDIOBJ h, unsigned type) {
  init();
  unsigned index = (h & 4095u);
  if (!index || index > SLOTS)
    return 0;
  if (slots[index - 1].generation != (h >> 12))
    return 0;
  struct object *o = slots[index - 1].object;
  return o && (!type || o->type == type) ? o : 0;
}
struct object *gdi_new(unsigned type, HGDIOBJ *h) {
  init();
  for (unsigned i = 21; i < SLOTS; i++)
    if (!slots[i].object && slots[i].generation < 0xfffff) {
      struct object *o = gdi_alloc(sizeof(*o));
      if (!o)
        return 0;
      o->type = type;
      slots[i].object = o;
      slots[i].generation++;
      *h = (slots[i].generation << 12) | (i + 1);
      return o;
    }
  return 0;
}
void gdi_ref(HGDIOBJ h, int delta) {
  struct object *o = gdi_object(h, 0);
  if (o && !o->stock) {
    o->refs = (unsigned)((int)o->refs + delta);
    if (o->type == GDI_BITMAP && !o->refs)
      o->u.bitmap.selected_dc = 0;
  }
}
struct dc *gdi_dc(HDC h) {
  struct object *o = gdi_object(h, 0);
  return o && (o->type == GDI_DC || o->type == GDI_MEMDC) ? &o->u.dc : 0;
}
struct w32_dib *gdi_target(struct dc *d) {
  if (!d)
    return 0;
  if (!d->memory)
    return &d->surface;
  struct object *o = gdi_object(d->s.bitmap, GDI_BITMAP);
  return o ? &o->u.bitmap.dib : 0;
}
DWORD gdi_color(COLORREF c) {
  return ((c & 255) << 16) | (c & 0xff00) | ((c >> 16) & 255);
}
DWORD gdi_device_bpp(struct dc *d) {
  return d->printer ? gdi_target(d)->bpp : desktop.bpp ? desktop.bpp : gdi_target(d)->bpp;
}
COLORREF gdi_colorref(DWORD c) { return gdi_color(c); }
HGDIOBJ W32API GetStockObject(int n) {
  GDI_GUARD;
  init();
  /* f5-10C: the font stocks are distinct realized logical fonts. */
  return n >= 0 && n < 20 && stocks[n].type ? stock_handle((unsigned)n) : 0;
}
static void state_refs(struct dc_state *s, int delta) {
  gdi_ref(s->pen, delta);
  gdi_ref(s->brush, delta);
  gdi_ref(s->font, delta);
  gdi_ref(s->bitmap, delta);
  gdi_ref(s->palette, delta);
}
static HDC new_dc(int memory, const struct w32_dib *surface) {
  HDC h;
  struct object *o = gdi_new(memory ? GDI_MEMDC : GDI_DC, &h);
  if (!o)
    return 0;
  struct dc *d = &o->u.dc;
  d->memory = memory;
  if (surface)
    d->surface = *surface;
  d->s = (struct dc_state){.pen = stock_handle(7),
                           .brush = stock_handle(0),
                           .font = stock_handle(13),
                           .bitmap = stock_handle(20),
                           .palette = stock_handle(15),
                           .bk = RGB(255, 255, 255),
                           .dcbrush = RGB(255, 255, 255),
                           .bk_mode = OPAQUE,
                           .rop2 = 13,
                           .stretch = COLORONCOLOR,
                           .fill = ALTERNATE,
                           .map = MM_TEXT,
                           .window_ext = {1, 1},
                           .viewport_ext = {1, 1}};
  return h;
}
HDC W32API CreateCompatibleDC(HDC ref) {
  GDI_GUARD;
  if (ref && !gdi_dc(ref))
    return 0;
  return new_dc(1, 0);
}
static int surface_layout(struct w32_dib *out, const struct w32_dib *in) {
  if (!in)
    return 0;
  *out = *in;
  if (!out->masks[0] && !out->masks[1] && !out->masks[2] && out->bpp >= 16) {
    out->masks[0] = out->bpp == 16 ? 0x7c00 : 0xff0000;
    out->masks[1] = out->bpp == 16 ? 0x3e0 : 0xff00;
    out->masks[2] = out->bpp == 16 ? 31 : 255;
  }
  return w32_dib_validate(out);
}
HDC W32API w32_gdi_surface_dc(const struct w32_dib *surface, POINT origin,
                              void (*damage)(void *, const RECT *),
                              void *opaque) {
  GDI_GUARD;
  struct w32_dib layout;
  if (!surface_layout(&layout, surface) || origin.x < -GDI_LIMIT ||
      origin.x > GDI_LIMIT || origin.y < -GDI_LIMIT || origin.y > GDI_LIMIT)
    return 0;
  HDC h = new_dc(0, &layout);
  struct dc *d = gdi_dc(h);
  if (d) {
    d->origin = origin;
    d->screen_origin = origin;
    d->damage = damage;
    d->opaque = opaque;
  }
  return h;
}
BOOL W32API w32_gdi_set_screen_origin(HDC h, POINT p) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d)
    return 0;
  d->screen_origin = p;
  return 1;
}
BOOL W32API w32_gdi_bind_surface(HDC h, const struct w32_dib *s) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  struct w32_dib layout;
  if (!d || d->memory || !surface_layout(&layout, s))
    return 0;
  d->surface = layout;
  return 1;
}
BOOL W32API w32_gdi_set_desktop(const struct w32_dib *s) {
  GDI_GUARD;
  struct w32_dib layout;
  if (!surface_layout(&layout, s))
    return 0;
  desktop = layout;
  return 1;
}
HDC W32API CreateDCA(const char *driver, const char *device, const char *output,
                     const void *mode) {
  GDI_GUARD;
  if (!driver || w32_stricmp(driver, "DISPLAY"))
    return gdi_printer_create(driver, device, output, mode, 0);
  if (output || !w32_dib_validate(&desktop))
    return 0;
  return new_dc(0, &desktop);
}
HDC W32API CreateDCW(const WCHAR *driver, const WCHAR *device,
                     const WCHAR *output, const void *mode) {
  char names[3][260];
  const WCHAR *input[3] = {driver, device, output};
  for (unsigned n = 0; n < 3; n++) {
    unsigned i = 0;
    if (input[n]) {
      for (; i < 259 && input[n][i]; i++) {
        int c = gdi_cp1252_ansi(input[n][i]);
        if (c < 0)
          return 0;
        names[n][i] = (char)c;
      }
      if (input[n][i])
        return 0;
    }
    names[n][i] = 0;
  }
  if (!driver || w32_stricmp(names[0], "DISPLAY")) {
    GDI_GUARD;
    return gdi_printer_create(driver ? names[0] : 0, device ? names[1] : 0,
                              output ? names[2] : 0, mode, 1);
  }
  return CreateDCA(names[0], device ? names[1] : 0, output ? names[2] : 0, mode);
}

BOOL W32API DeleteDC(HDC h) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d)
    return 0;
  gdi_printer_free(d);
  gdi_record_free(d);
  state_refs(&d->s, -1);
  gdi_region_free(&d->s.clip);
  gdi_region_free(&d->visible);
  gdi_region_free(&d->shape);
  while (d->saved) {
    struct saved *s = d->saved;
    d->saved = s->next;
    state_refs(&s->state, -1);
    gdi_region_free(&s->state.clip);
    gdi_free(s, sizeof(*s));
  }
  struct object *o = gdi_object(h, 0);
  slots[(h & 4095) - 1].object = 0;
  gdi_free(o, sizeof(*o));
  return 1;
}
int W32API SaveDC(HDC h) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d || d->level >= 1024)
    return 0;
  gdi_record(d, 0x001eu, 0, 0);
  struct saved *s = gdi_alloc(sizeof(*s));
  if (!s)
    return 0;
  s->state = d->s;
  s->state.clip = (struct region){0};
  if (!gdi_region_copy(&s->state.clip, &d->s.clip)) {
    gdi_free(s, sizeof(*s));
    return 0;
  }
  state_refs(&s->state, 1);
  s->level = ++d->level;
  s->next = d->saved;
  d->saved = s;
  return s->level;
}
BOOL W32API RestoreDC(HDC h, int index) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d || !index)
    return 0;
  if (d->record) {
    int16_t v[1] = {(int16_t)index};
    gdi_record(d, 0x0127u, v, 1);
  }
  int target = index < 0 ? d->level + 1 + index : index;
  if (target <= 0 || target > d->level)
    return 0;
  while (d->saved) {
    struct saved *s = d->saved;
    d->saved = s->next;
    if (s->level == target) {
      state_refs(&d->s, -1);
      gdi_region_free(&d->s.clip);
      d->s = s->state;
      d->level = target - 1;
      gdi_free(s, sizeof(*s));
      return 1;
    }
    state_refs(&s->state, -1);
    gdi_region_free(&s->state.clip);
    gdi_free(s, sizeof(*s));
  }
  return 0;
}
HGDIOBJ W32API SelectObject(HDC h, HGDIOBJ obj) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  struct object *o = gdi_object(obj, 0);
  if (!d || !o)
    return 0;
  if (o->type == GDI_REGION)
    return (HGDIOBJ)SelectClipRgn(h, obj);
  if (d->record) /* f5-20B metafile DC */
    gdi_record_select(h, d, obj);
  HGDIOBJ *p = o->type == GDI_PEN                   ? &d->s.pen
               : o->type == GDI_BRUSH               ? &d->s.brush
               : o->type == GDI_FONT                ? &d->s.font
               : o->type == GDI_BITMAP && d->memory ? &d->s.bitmap
                                                    : 0;
  if (!p || (o->type == GDI_BITMAP && !o->stock && o->u.bitmap.selected_dc &&
             o->u.bitmap.selected_dc != h))
    return 0;
  HGDIOBJ old = *p;
  gdi_ref(obj, 1);
  if (o->type == GDI_BITMAP && !o->stock)
    o->u.bitmap.selected_dc = h;
  gdi_ref(old, -1);
  *p = obj;
  return old;
}
HGDIOBJ W32API GetCurrentObject(HDC h, unsigned type) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d)
    return 0;
  return type == GDI_PEN       ? d->s.pen
         : type == GDI_BRUSH   ? d->s.brush
         : type == GDI_FONT    ? d->s.font
         : type == GDI_BITMAP  ? d->s.bitmap
         : type == GDI_PALETTE ? d->s.palette
                               : 0;
}
DWORD W32API GetObjectType(HGDIOBJ h) {
  GDI_GUARD;
  struct object *o = gdi_object(h, 0);
  /* f5-20B: a metafile DC reports OBJ_METADC. */
  return o ? (o->type == GDI_MEMDC && o->u.dc.record ? 4u : o->type) : 0;
}
#if defined(_WIN32) && !defined(W32_HOST_MODEL)
__declspec(dllimport) BOOL W32API UnmapViewOfFile(const void *);
#else
extern BOOL W32API UnmapViewOfFile(const void *);
#endif
BOOL W32API DeleteObject(HGDIOBJ h) {
  GDI_GUARD;
  struct object *o = gdi_object(h, 0);
  if (!o || o->refs || o->type == GDI_DC || o->type == GDI_MEMDC)
    return 0;
  if (o->stock)
    return 1;
  if (o->type == GDI_BITMAP) {
    if (o->u.bitmap.section)
      UnmapViewOfFile(o->u.bitmap.allocation);
    else
      gdi_free(o->u.bitmap.allocation, o->u.bitmap.alloc_size);
  }
  if (o->type == GDI_REGION)
    gdi_region_free(&o->u.region);
  if (o->type == GDI_METAFILE || o->type == GDI_ENHMETAFILE)
    gdi_free(o->u.meta.bits, o->u.meta.size);
  if (o->type == GDI_PALETTE)
    UnrealizeObject(h);
  if (o->type == GDI_FONT && o->u.font.provider.release)
    o->u.font.provider.release(o->u.font.face);
  slots[(h & 4095) - 1].object = 0;
  gdi_free(o, sizeof(*o));
  return 1;
}
DWORD W32API w32_gdi_live_objects(void) {
  GDI_GUARD;
  DWORD n = 0;
  for (unsigned i = 21; i < SLOTS; i++)
    if (slots[i].object)
      n++;
  return n;
}
DWORD W32API w32_gdi_surface_bytes(void) {
  GDI_GUARD;
  DWORD bytes = 0;
  for (unsigned i = 21; i < SLOTS; i++)
    if (slots[i].object && slots[i].object->type == GDI_BITMAP)
      bytes += slots[i].object->u.bitmap.dib.size;
  return bytes;
}
void gdi_palette_targets(void) {
  DWORD colors[256];
  gdi_system_palette(colors);
  for (unsigned i = 21; i < SLOTS; i++) {
    struct object *o = slots[i].object;
    if (!o || (o->type != GDI_DC && o->type != GDI_MEMDC))
      continue;
    struct dc *d = &o->u.dc;
    struct w32_dib *s = gdi_target(d);
    if (!s || s->bpp != 8)
      continue;
    struct object *bitmap = d->memory ? gdi_object(d->s.bitmap, GDI_BITMAP) : 0;
    if (bitmap && bitmap->u.bitmap.dib_section)
      continue;
    w32_copy(s->colors, colors, sizeof(colors));
    s->color_count = 256;
    gdi_damage(d, (RECT){0, 0, (LONG)s->width, (LONG)s->height});
  }
}
void W32API w32_gdi_process_exit(void) {
  GDI_GUARD;
  init();
  for (unsigned i = 21; i < SLOTS; i++)
    if (slots[i].object &&
        (slots[i].object->type == GDI_DC || slots[i].object->type == GDI_MEMDC))
      DeleteDC((slots[i].generation << 12) | (i + 1));
  for (unsigned i = 21; i < SLOTS; i++)
    if (slots[i].object)
      DeleteObject((slots[i].generation << 12) | (i + 1));
  static const unsigned fonts[] = {10, 11, 12, 13, 14, 16, 17};
  for (unsigned i = 0; i < sizeof(fonts) / sizeof(*fonts); i++) {
    struct font *font = &stocks[fonts[i]].u.font;
    if (font->provider.release)
      font->provider.release(font->face);
    *font = (struct font){0};
  }
  gdi_font_exit();
  for (unsigned i = 0; i < sizeof(fonts) / sizeof(*fonts); i++)
    gdi_font_stock(fonts[i], &stocks[fonts[i]].u.font);
  gdi_printer_exit();
  desktop = (struct w32_dib){0};
  gdi_palette_reset();
}
BOOL W32API GdiDllMain(void *module, DWORD reason, void *reserved) {
  (void)module;
  (void)reserved;
  if (!reason)
    w32_gdi_process_exit();
  return 1;
}
int W32API GetObjectA(HGDIOBJ h, int bytes, void *out) {
  GDI_GUARD;
  struct object *o = gdi_object(h, 0);
  if (!o)
    return 0;
  if (o->type == GDI_FONT)
    return o->u.font.provider.get_object
               ? o->u.font.provider.get_object(o->u.font.face, bytes, out, 0)
               : 0;
  const void *p = 0;
  int size = 0;
  DIBSECTION ds = {0};
  uint16_t count;
  if (o->type == GDI_BITMAP) {
    struct bitmap *b = &o->u.bitmap;
    struct w32_dib *d = &b->dib;
    ds.dsBm = (BITMAP){0,
                       (LONG)d->width,
                       (LONG)d->height,
                       (LONG)d->pitch,
                       1,
                       (uint16_t)d->bpp,
                       b->dib_section ? d->bits : 0};
    ds.dsBmih = (BITMAPINFOHEADER){
        40,
        (LONG)d->width,
        d->top_down ? -(LONG)d->height : (LONG)d->height,
        1,
        (uint16_t)d->bpp,
        d->bpp == 16 && (d->masks[0] != 0x7c00 || d->masks[1] != 0x3e0 ||
                         d->masks[2] != 31)
            ? BI_BITFIELDS
            : BI_RGB,
        d->size,
        0,
        0,
        d->bpp <= 8 ? d->color_count : 0,
        0};
    for (unsigned i = 0; i < 3; i++)
      ds.dsBitfields[i] = d->masks[i];
    ds.dshSection = b->section;
    ds.dsOffset = b->offset;
    size = b->dib_section && bytes >= (int)sizeof(ds) ? sizeof(ds)
                                                      : sizeof(BITMAP);
    p = &ds;
  } else if (o->type == GDI_PEN) {
    p = &o->u.pen;
    size = sizeof(LOGPEN);
  } else if (o->type == GDI_BRUSH) {
    p = &o->u.brush.log;
    size = sizeof(LOGBRUSH);
  } else if (o->type == GDI_PALETTE) {
    count = (uint16_t)o->u.palette.count;
    p = &count;
    size = sizeof(count);
  }
  if (!out)
    return size;
  if (bytes < size || !size)
    return 0;
  w32_copy(out, p, (DWORD)size);
  return size;
}
int W32API GetObjectW(HGDIOBJ h, int bytes, void *out) {
  GDI_GUARD;
  struct object *o = gdi_object(h, GDI_FONT);
  if (o)
    return o->u.font.provider.get_object
               ? o->u.font.provider.get_object(o->u.font.face, bytes, out, 1)
               : 0;
  return GetObjectA(h, bytes, out);
}
/* Windows 98 font DPI is established when a GUI process starts. */
static unsigned display_effects=AP_SMOOTH;
static unsigned display_dpi(void) {
  static unsigned dpi; static DWORD last;
  DWORD now=GetTickCount();
  if(dpi && (DWORD)(now-last)<1000) return dpi;
  if(!dpi) dpi=96;
  last=now;
  char *text=gdi_alloc(32769); if(!text) return dpi;
  void *file=CreateFileA("C:\\system\\settings\\desktop.cfg",0x80000000u,7,0,3,0,0);
  DWORD n=0;
  if(file!=(void *)(intptr_t)-1) {
    if(ReadFile(file,text,32768,&n,0) && n<=32768) {
      text[n]=0;
      struct appearance a;
      if(ap_settings_decode(&a,text,n)) { dpi=a.dpi; display_effects=a.effects; }
    }
    CloseHandle(file);
  }
  gdi_free(text,32769); return dpi;
}
int gdi_smoothing(void) { (void)display_dpi(); return !!(display_effects&AP_SMOOTH); }
/* f7: a display DC (the screen DC of GetDC(NULL), a window DC or a memory
 * DC compatible with the display) reports the display device, as Windows
 * GetDeviceCaps does for HORZRES, VERTRES and BITSPIXEL; not the 1x1
 * monochrome default bitmap of a memory DC or the window surface. USER owns
 * the display query (w32_user_display_caps: width, height, bits per pixel
 * as programs see the composed desktop); read at most every 250 ms. */
#ifdef W32_HOST_MODEL
extern BOOL W32API w32_user_display_caps(DWORD *) __attribute__((weak));
#else
extern void *W32API GetModuleHandleA(const char *);
extern void *W32API GetProcAddress(void *, const char *);
#endif
static int display_caps(DWORD *out) {
  static DWORD cached[3], last;
  static int valid;
  DWORD now = GetTickCount();
  if (valid && (DWORD)(now - last) < 250) {
    w32_copy(out, cached, sizeof(cached));
    return 1;
  }
  BOOL(W32API * query)(DWORD *) = 0;
#ifdef W32_HOST_MODEL
  query = w32_user_display_caps;
#else
  void *m = GetModuleHandleA("user32.dll");
  query = m ? GetProcAddress(m, "w32_user_display_caps") : 0;
#endif
  DWORD v[3] = {0, 0, 0};
  if (!query || !query(v) || !v[0] || !v[1] || !v[2])
    return 0;
  w32_copy(cached, v, sizeof(v));
  last = now;
  valid = 1;
  w32_copy(out, v, sizeof(v));
  return 1;
}
int W32API GetDeviceCaps(HDC h, int cap) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  struct w32_dib *s = gdi_target(d);
  if (!s)
    return 0;
  if (d->printer)
    return gdi_printer_caps(d, cap);
  DWORD width = desktop.width ? desktop.width : s->width,
        height = desktop.height ? desktop.height : s->height,
        bpp = desktop.bpp ? desktop.bpp : s->bpp, shown[3];
  if (!desktop.bpp && (cap == 4 || cap == 6 || cap == 8 || cap == 10 || cap == 12 ||
                       cap == 24 || cap == 38 || cap == 104 || cap == 106 ||
                       cap == 108 || cap == 117 || cap == 118) &&
      display_caps(shown)) {
    width = shown[0];
    height = shown[1];
    bpp = shown[2];
  }
  switch (cap) {
  case 0:
    return 0x400;
  case 2:
    return 1;
  case 4:
    return (int)(width * 254 / (display_dpi()*10));
  case 6:
    return (int)(height * 254 / (display_dpi()*10));
  case 8:
    return (int)width;
  case 10:
    return (int)height;
  case 12:
    return (int)bpp;
  case 14:
    return 1;
  case 16:
  case 18:
  case 20:
  case 22:
    return -1;
  case 24:
    return bpp <= 8 ? (1 << bpp) : -1;
  case 26:
    return 0;
  case 28:
    return 0x3e;
  case 30:
    return 0x1e;
  case 32:
    return 0x7f;
  case 34:
    return 0x1ff;
  case 38:
    return bpp <= 8 ? 0xbf99 : 0xbe99;
  case 88:
  case 90:
    return (int)display_dpi();
  case 104:
    return bpp <= 8 ? 256 : 0;
  case 106:
    return bpp <= 8 ? 20 : 0;
  case 108:
    return (int)bpp;
  case 116:
    return 0; /* VBE firmware default; an EDID timing is not scanout state. */
  case 117:
    return (int)height;
  case 118:
    return (int)width;
  default:
    return 0;
  }
}
BOOL W32API GetDCOrgEx(HDC h, POINT *p) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d || !p)
    return 0;
  *p = d->screen_origin;
  return 1;
}
void gdi_damage(struct dc *d, RECT r) {
  struct w32_dib *s = gdi_target(d);
  if (!s || !d->damage)
    return;
  if (r.left < 0)
    r.left = 0;
  if (r.top < 0)
    r.top = 0;
  if (r.right > (LONG)s->width)
    r.right = (LONG)s->width;
  if (r.bottom > (LONG)s->height)
    r.bottom = (LONG)s->height;
  if (r.left < r.right && r.top < r.bottom)
    d->damage(d->opaque, &r);
}
#define COLOR_ACCESS(Name, field, meta)                                      \
  COLORREF W32API Set##Name(HDC h, COLORREF c) {                               \
    GDI_GUARD;                                                                 \
    struct dc *d = gdi_dc(h);                                                  \
    if (!d)                                                                    \
      return CLR_INVALID;                                                      \
    if (meta && d->record) { /* f5-20B metafile DC */                        \
      int16_t v[2] = {(int16_t)(c & 0xffff), (int16_t)(c >> 16)};              \
      gdi_record(d, meta, v, 2);                                             \
    }                                                                          \
    COLORREF old = d->s.field;                                                 \
    d->s.field = c;                                                            \
    return old;                                                                \
  }                                                                            \
  COLORREF W32API Get##Name(HDC h) {                                           \
    GDI_GUARD;                                                                 \
    struct dc *d = gdi_dc(h);                                                  \
    return d ? d->s.field : CLR_INVALID;                                       \
  }
COLOR_ACCESS(BkColor, bk, 0x0201u)
COLOR_ACCESS(TextColor, text, 0x0209u)
COLOR_ACCESS(DCBrushColor, dcbrush, 0)
COLOR_ACCESS(DCPenColor, dcpen, 0)
#define INT_ACCESS(Name, field, lo, hi, meta)                                \
  int W32API Set##Name(HDC h, int value) {                                     \
    GDI_GUARD;                                                                 \
    struct dc *d = gdi_dc(h);                                                  \
    if (!d || value < lo || value > hi)                                        \
      return 0;                                                                \
    if (d->record) {                                                           \
      int16_t v[1] = {(int16_t)value};                                         \
      gdi_record(d, meta, v, 1);                                             \
    }                                                                          \
    int old = d->s.field;                                                      \
    d->s.field = value;                                                        \
    return old;                                                                \
  }                                                                            \
  int W32API Get##Name(HDC h) {                                                \
    GDI_GUARD;                                                                 \
    struct dc *d = gdi_dc(h);                                                  \
    return d ? d->s.field : 0;                                                 \
  }
INT_ACCESS(BkMode, bk_mode, 1, 2, 0x0102u)
INT_ACCESS(ROP2, rop2, 1, 16, 0x0104u)
INT_ACCESS(StretchBltMode, stretch, 1, 4, 0x0107u)
INT_ACCESS(PolyFillMode, fill, 1, 2, 0x0106u)
int W32API GetMapMode(HDC h) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  return d ? d->s.map : 0;
}
int W32API SetMapMode(HDC h, int mode) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d || mode < 1 || mode > 8)
    return 0;
  if (d->record) {
    int16_t v[1] = {(int16_t)mode};
    gdi_record(d, 0x0103u, v, 1);
  }
  int old = d->s.map;
  d->s.map = mode;
  if (mode <= MM_TWIPS) {
    static const int units[] = {1, 1, 254, 2540, 100, 1000, 1440};
    int u = units[mode];
    d->s.window_ext = (SIZE){u, u};
    d->s.viewport_ext =
        (SIZE){mode == MM_TEXT ? 1 : 96, mode == MM_TEXT ? 1 : -96};
  }
  return old;
}
static void isotropic(struct dc *d) {
  if (d->s.map != MM_ISOTROPIC)
    return;
  int64_t wx = d->s.window_ext.cx, wy = d->s.window_ext.cy,
          vx = d->s.viewport_ext.cx, vy = d->s.viewport_ext.cy;
  int sx = vx < 0 ? -1 : 1, sy = vy < 0 ? -1 : 1;
  if (wx < 0)
    wx = -wx;
  if (wy < 0)
    wy = -wy;
  if (vx < 0)
    vx = -vx;
  if (vy < 0)
    vy = -vy;
  if (vx * wy > vy * wx)
    d->s.viewport_ext.cx = (LONG)(sx * (vy * wx / wy));
  else
    d->s.viewport_ext.cy = (LONG)(sy * (vx * wy / wx));
  if (!d->s.viewport_ext.cx)
    d->s.viewport_ext.cx = sx;
  if (!d->s.viewport_ext.cy)
    d->s.viewport_ext.cy = sy;
}
#define POINT_ACCESS(Name, field, meta)                                      \
  BOOL W32API Set##Name##Ex(HDC h, int x, int y, POINT *old) {                 \
    GDI_GUARD;                                                                 \
    struct dc *d = gdi_dc(h);                                                  \
    if (!d || x < -GDI_LIMIT || x > GDI_LIMIT || y < -GDI_LIMIT ||             \
        y > GDI_LIMIT)                                                         \
      return 0;                                                                \
    if (meta && d->record) {                                                 \
      int16_t v[2] = {(int16_t)y, (int16_t)x};                                 \
      gdi_record(d, meta, v, 2);                                             \
    }                                                                          \
    if (old)                                                                   \
      *old = d->s.field;                                                       \
    d->s.field = (POINT){x, y};                                                \
    return 1;                                                                  \
  }                                                                            \
  BOOL W32API Get##Name##Ex(HDC h, POINT *p) {                                 \
    GDI_GUARD;                                                                 \
    struct dc *d = gdi_dc(h);                                                  \
    if (!d || !p)                                                              \
      return 0;                                                                \
    *p = d->s.field;                                                           \
    return 1;                                                                  \
  }
POINT_ACCESS(ViewportOrg, viewport_org, 0x020du)
POINT_ACCESS(WindowOrg, window_org, 0x020bu)
POINT_ACCESS(BrushOrg, brush_org, 0)
#define SIZE_ACCESS(Name, field, meta)                                       \
  BOOL W32API Set##Name##Ex(HDC h, int x, int y, SIZE *old) {                  \
    GDI_GUARD;                                                                 \
    struct dc *d = gdi_dc(h);                                                  \
    if (!d || !x || !y || x < -GDI_LIMIT || x > GDI_LIMIT || y < -GDI_LIMIT || \
        y > GDI_LIMIT)                                                         \
      return 0;                                                                \
    if (d->record) {                                                           \
      int16_t v[2] = {(int16_t)y, (int16_t)x};                                 \
      gdi_record(d, meta, v, 2);                                             \
    }                                                                          \
    if (old)                                                                   \
      *old = d->s.field;                                                       \
    if (d->s.map >= MM_ISOTROPIC) {                                            \
      d->s.field = (SIZE){x, y};                                               \
      isotropic(d);                                                            \
    }                                                                          \
    return 1;                                                                  \
  }                                                                            \
  BOOL W32API Get##Name##Ex(HDC h, SIZE *p) {                                  \
    GDI_GUARD;                                                                 \
    struct dc *d = gdi_dc(h);                                                  \
    if (!d || !p)                                                              \
      return 0;                                                                \
    *p = d->s.field;                                                           \
    return 1;                                                                  \
  }
SIZE_ACCESS(ViewportExt, viewport_ext, 0x020eu)
SIZE_ACCESS(WindowExt, window_ext, 0x020cu)
static int transform_one(struct dc *d, POINT p, int reverse, POINT *out,
                         int buffer) {
  POINT a = reverse ? d->s.viewport_org : d->s.window_org,
        b = reverse ? d->s.window_org : d->s.viewport_org;
  SIZE from = reverse ? d->s.viewport_ext : d->s.window_ext,
       to = reverse ? d->s.window_ext : d->s.viewport_ext;
  int64_t ox = buffer ? d->origin.x : 0, oy = buffer ? d->origin.y : 0;
  int64_t x = ((int64_t)p.x - a.x - (reverse ? ox : 0)) * to.cx / from.cx +
              b.x + (reverse ? 0 : ox),
          y = ((int64_t)p.y - a.y - (reverse ? oy : 0)) * to.cy / from.cy +
              b.y + (reverse ? 0 : oy);
  if (x < -GDI_LIMIT || x > GDI_LIMIT || y < -GDI_LIMIT || y > GDI_LIMIT)
    return 0;
  *out = (POINT){(LONG)x, (LONG)y};
  return 1;
}
static int map_points(struct dc *d, POINT *p, int count, int reverse,
                      int buffer) {
  if (!d || !p || count < 0 || count > 65536)
    return 0;
  POINT temp;
  for (int i = 0; i < count; i++)
    if (!transform_one(d, p[i], reverse, &temp, buffer))
      return 0;
  for (int i = 0; i < count; i++) {
    transform_one(d, p[i], reverse, &temp, buffer);
    p[i] = temp;
  }
  return 1;
}
int gdi_transform(struct dc *d, POINT *p, int count, int reverse) {
  return map_points(d, p, count, reverse, 1);
}
BOOL W32API LPtoDP(HDC h, POINT *p, int n) {
  GDI_GUARD;
  return map_points(gdi_dc(h), p, n, 0, 0);
}
BOOL W32API DPtoLP(HDC h, POINT *p, int n) {
  GDI_GUARD;
  return map_points(gdi_dc(h), p, n, 1, 0);
}
RECT gdi_rect(struct dc *d, RECT r) {
  POINT p[2] = {{r.left, r.top}, {r.right, r.bottom}};
  if (!gdi_transform(d, p, 2, 0))
    return (RECT){0};
  return (RECT){
      p[0].x < p[1].x ? p[0].x : p[1].x, p[0].y < p[1].y ? p[0].y : p[1].y,
      p[0].x > p[1].x ? p[0].x : p[1].x, p[0].y > p[1].y ? p[0].y : p[1].y};
}
