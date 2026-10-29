/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "ciuki-icon32.h"
#include "unicode.h"
#include "../../../apps/libdeskui/iconx.h"
/* fx-exeicons: small[] is the icon's own 16x16 image (straight ARGB) when its
 * group has one (LoadIcon, ExtractIcon, the class and window icons the
 * desktop shows in captions and on the taskbar); USER draws the 32x32
 * colour image through its AND mask, as Windows 98 does. */
struct icon {
  DWORD id, resource;
  BOOL cursor, shared;
  UINT hotx, hoty;
  DWORD pixels[1024];
  uint8_t mask[1024];
  BOOL has_small;
  DWORD small[256];
};
static struct icon icons[64];
static DWORD serial = 0x10000;
static HCURSOR current;
static int cursor_count;
static DWORD cached[16][4], cached_generation[16];
void u32_icons_cleanup(void) {
  w32_fill(icons, 0, sizeof(icons));
  w32_fill(cached, 0, sizeof(cached));
  w32_fill(cached_generation, 0, sizeof(cached_generation));
  current = 0; cursor_count = 0;
}
extern HANDLE W32API FindResourceA(HANDLE, const void *, const void *);
extern HANDLE W32API LoadResource(HANDLE, HANDLE);
extern DWORD W32API SizeofResource(HANDLE, HANDLE);
extern HANDLE W32API GetModuleHandleA(const char *);
extern void *W32API GetProcAddress(HANDLE, const char *);
static struct icon *find(HANDLE h) {
  for (UINT i = 0; i < 64; i++)
    if (icons[i].id == U32_PTR(h))
      return &icons[i];
  return 0;
}
BOOL u32_tray_pixels(HICON handle,struct u32_surface *s) {
  struct icon *i=find(handle);
  if (handle && (!i || i->cursor)) return u32_fail(1402);
  if (!u32_backend.surface(16,16,s)) return 0;
  for (unsigned y=0;y<16;y++) for (unsigned x=0;x<16;x++) {
    unsigned at=y*64+x*2;
    w32_put32((uint8_t *)s->pixels+y*s->pitch+x*4,i && !i->mask[at] ? i->pixels[at] : 0);
  }
  return 1;
}
static struct icon *allocate(BOOL cursor) {
  for (UINT i = 0; i < 64; i++)
    if (!icons[i].id) {
      w32_fill(&icons[i], 0, sizeof(icons[i]));
      icons[i].id = ++serial;
      icons[i].cursor = cursor;
      return &icons[i];
    }
  u32_fail(8);
  return 0;
}
static UINT shape(HCURSOR h) {
  switch (U32_PTR(h)) {
  case 32513:
    return DESK_CURSOR_TEXT;
  case 32514:
    return DESK_CURSOR_BUSY;
  case 32515:
    return DESK_CURSOR_CROSSHAIR;
  case 32642:
    return DESK_CURSOR_RESIZE_NWSE;
  case 32643:
    return DESK_CURSOR_RESIZE_NESW;
  case 32644:
    return DESK_CURSOR_RESIZE_EW;
  case 32645:
    return DESK_CURSOR_RESIZE_NS;
  case 32646:
    return DESK_CURSOR_MOVE;
  case 32648:
    return DESK_CURSOR_FORBIDDEN;
  case 32649:
    return DESK_CURSOR_HAND;
  case 32650:
    return DESK_CURSOR_ARROW_BUSY;
  default:
    return DESK_CURSOR_ARROW;
  }
}
static BOOL publish(void) {
  struct u32_window *w = u32_root(u32_window(u32_foreground));
  if (!w)
    return 1;
  UINT id = 0;
  struct icon *i = find(current);
  if (i) {
    UINT slot = w->wire - 1;
    if (cached_generation[slot] != w->generation) {
      w32_fill(cached[slot], 0, sizeof(cached[slot]));
      cached_generation[slot] = w->generation;
    }
    for (UINT k = 0; k < 4; k++)
      if (cached[slot][k] == i->id)
        id = k + 1;
    if (!id) {
      for (UINT k = 0; k < 4; k++)
        if (!cached[slot][k]) {
          id = k + 1;
          break;
        }
      if (!id)
        id = 1;
      struct u32_surface s = {.fd = -1};
      if (!u32_backend.surface(32, 32, &s))
        return 0;
      /* The XOR image keeps its colour under AND=1 for DrawIconEx; the
       * desktop cursor has no screen-inversion mode, so those are clear. */
      for (UINT y = 0; y < 32; y++)
        for (UINT x = 0; x < 32; x++)
          w32_put32((uint8_t *)s.pixels + y * s.pitch + x * 4,
                    i->mask[y * 32 + x] ? 0 : i->pixels[y * 32 + x]);
      uint8_t b[12];
      desk_put32(b, id);
      desk_put32(b + 4, i->hotx);
      desk_put32(b + 8, i->hoty);
      BOOL ok = u32_wire(w, DESK_V3_CURSOR_BITMAP, b, 12, s.fd);
      u32_backend.drop_surface(&s);
      if (!ok)
        return 0;
      cached[slot][id - 1] = i->id;
    }
  }
  uint8_t b[12];
  desk_put32(b, shape(current));
  desk_put32(b + 4, id);
  desk_put32(b + 8, current ? cursor_count : -1);
  return u32_wire(w, DESK_SET_CURSOR, b, 12, -1);
}
int W32API ShowCursor(BOOL show) {
  U32_GUARD;
  int before = cursor_count;
  if (show && cursor_count < 32767)
    cursor_count++;
  if (!show && cursor_count > -32768)
    cursor_count--;
  /* fx-hlrun: the cursor is shown while the display count is >= 0
   * (Microsoft ShowCursor); the desktop uses only that, so a call that keeps
   * the visibility sends nothing. Half-Life's launcher calls ShowCursor(TRUE)
   * and ShowCursor(FALSE) on every pass of its idle loop. */
  if ((before >= 0) != (cursor_count >= 0))
    publish();
  return cursor_count;
}
HCURSOR W32API SetCursor(HCURSOR h) {
  U32_GUARD;
  if (h && U32_PTR(h) < 0x10000) {
    UINT n = U32_PTR(h);
    if (n < 32512 || n > 32651) {
      u32_fail(6);
      return 0;
    }
  } else if (h && !find(h)) {
    u32_fail(6);
    return 0;
  }
  HCURSOR old = current;
  current = h;
  if (!publish())
    current = old;
  return old;
}
HCURSOR W32API GetCursor(void) {
  U32_GUARD;
  return current;
}
HCURSOR W32API CreateCursor(HINSTANCE instance, int x, int y, int width,
                            int height, const void *and_bits,
                            const void *xor_bits) {
  U32_GUARD;
  (void)instance;
  if (width != 32 || height != 32 || x < 0 || x >= 32 || y < 0 || y >= 32 ||
      !and_bits || !xor_bits) {
    u32_fail(87);
    return 0;
  }
  struct icon *i = allocate(1);
  if (!i)
    return 0;
  i->hotx = x;
  i->hoty = y;
  const uint8_t *a = and_bits, *b = xor_bits;
  for (UINT p = 0; p < 1024; p++) {
    UINT bit = 7 - (p & 7), at = p / 8, mask = (a[at] >> bit) & 1,
         xor = (b[at] >> bit) & 1;
    if (mask && xor) {
      w32_fill(i, 0, sizeof(*i));
      u32_fail(120);
      return 0;
    }
    i->mask[p] = mask;
    i->pixels[p] = mask ? 0 : 0xff000000u | (xor ? 0xffffff : 0);
  }
  return (HCURSOR)(uintptr_t)i->id;
}
BOOL W32API DestroyCursor(HCURSOR h) {
  U32_GUARD;
  struct icon *i = find(h);
  if (!i || !i->cursor)
    return u32_fail(6);
  if (i->shared)
    return 1;
  if (current == h)
    SetCursor((HCURSOR)(uintptr_t)32512);
  w32_fill(i, 0, sizeof(*i));
  return 1;
}
BOOL W32API DestroyIcon(HICON h) {
  U32_GUARD;
  struct icon *i = find(h);
  if (!i || i->cursor)
    return u32_fail(6);
  if (!i->shared)
    w32_fill(i, 0, sizeof(*i));
  return 1;
}
/* f5-20B: one RT_ICON/RT_CURSOR image (BITMAPINFOHEADER, XOR image, AND
 * mask; cursors start with the hotspot) with each dimension up to 256
 * pixels and 1/4/8/24/32 bits, resampled to USER's 32x32 icon by nearest
 * neighbour. PNG images (Windows Vista) are refused as on Windows 98. */
static struct icon *from_bits(const uint8_t *b, DWORD n, BOOL cursor) {
  UINT hotx = 0, hoty = 0;
  if (!b)
    return (struct icon *)(uintptr_t)u32_fail(87);
  if (cursor) {
    if (n < 4)
      return (struct icon *)(uintptr_t)u32_fail(13);
    hotx = w32_u16(b);
    hoty = w32_u16(b + 2);
    b += 4;
    n -= 4;
  }
  if (n < 40 || w32_u32(b) != 40 || w32_u16(b + 12) != 1 || w32_u32(b + 16))
    return (struct icon *)(uintptr_t)u32_fail(13);
  LONG width = (LONG)w32_u32(b + 4), height2 = (LONG)w32_u32(b + 8);
  UINT depth = w32_u16(b + 14);
  if (width <= 0 || width > 256 || height2 <= 0 || height2 > 512 || (height2 & 1) ||
      (depth != 1 && depth != 4 && depth != 8 && depth != 24 && depth != 32))
    return (struct icon *)(uintptr_t)u32_fail(13);
  UINT size = (UINT)width, height = (UINT)height2 / 2;
  UINT colors =
      depth <= 8 ? (w32_u32(b + 32) ? w32_u32(b + 32) : 1u << depth) : 0;
  if (colors > 256)
    return (struct icon *)(uintptr_t)u32_fail(13);
  UINT pitch = ((size * depth + 31) / 32) * 4, offset = 40 + colors * 4,
       mask_pitch = ((size + 31) / 32) * 4;
  if ((uint64_t)offset + (uint64_t)pitch * height + (uint64_t)mask_pitch * height >
      n)
    return (struct icon *)(uintptr_t)u32_fail(13);
  struct icon *i = allocate(cursor);
  if (!i)
    return 0;
  i->hotx = size == 32 ? hotx : hotx * 32 / size;
  i->hoty = height == 32 ? hoty : hoty * 32 / height;
  for (UINT y = 0; y < 32; y++)
    for (UINT x = 0; x < 32; x++) {
      UINT sx = x * size / 32, sy = y * height / 32;
      const uint8_t *row = b + offset + (height - 1 - sy) * pitch;
      UINT p = y * 32 + x;
      DWORD c = 0;
      if (depth == 32)
        c = w32_u32(row + sx * 4);
      else if (depth == 24)
        c = row[sx * 3] | (DWORD)row[sx * 3 + 1] << 8 |
            (DWORD)row[sx * 3 + 2] << 16;
      else {
        UINT idx = depth == 8   ? row[sx]
                   : depth == 4 ? (row[sx / 2] >> (sx & 1 ? 0 : 4)) & 15
                                : (row[sx / 8] >> (7 - (sx & 7))) & 1;
        if (idx >= colors) {
          i->id = 0;
          return (struct icon *)(uintptr_t)u32_fail(13);
        }
        c = w32_u32(b + 40 + idx * 4);
      }
      BOOL mask = !!(b[offset + pitch * height + (height - 1 - sy) * mask_pitch +
                       sx / 8] &
                     (128 >> (sx & 7)));
      i->mask[p] = mask;
      i->pixels[p] = mask ? 0 : (c & 0xffffff) | 0xff000000;
    }
  return i;
}
/* An iconx image (straight ARGB, 32x32) as USER's colour image and AND
 * mask: alpha below 128 is transparent. */
static void icon_set_argb(struct icon *i, const uint32_t *argb) {
  for (UINT p = 0; p < 1024; p++) {
    BOOL m = (argb[p] >> 24) < 128;
    i->mask[p] = m;
    i->pixels[p] = m ? 0 : argb[p] | 0xff000000u;
  }
}
/* The images of an in-memory RT_GROUP_ICON ranked for a size (iconx: the
 * closest size, then the deepest colour), decoded through the module's
 * RT_ICON resources. */
static BOOL group_image(HANDLE module, const uint8_t *g, DWORD gn, UINT size, uint32_t *out) {
  unsigned order[ICONX_TRIES];
  int n = iconx_rank(g, gn, 14, size, order, ICONX_TRIES);
  struct iconx *x = n > 0 ? u32_alloc(sizeof(*x)) : 0;
  BOOL ok = 0;
  for (int k = 0; x && k < n && !ok; k++) {
    UINT rid = w32_u16(g + 6 + order[k] * 14 + 12);
    HANDLE r = FindResourceA(module, (void *)(uintptr_t)rid, (void *)(uintptr_t)3);
    const uint8_t *b = r ? LoadResource(module, r) : 0;
    if (!b)
      continue;
    struct iconx_memory m = {b, SizeofResource(module, r)};
    iconx_init(x, iconx_memory_read, &m, m.size);
    ok = !iconx_decode(x, 0, m.size, size, out);
  }
  if (x)
    u32_free(x);
  return ok;
}
static void group_small(HANDLE module, const uint8_t *g, DWORD gn, struct icon *i) {
  i->has_small = group_image(module, g, gn, 16, i->small);
}
static HANDLE load(HINSTANCE instance, const char *name, BOOL cursor) {
  if (!instance) {
    if (cursor) {
      UINT n = U32_PTR(name);
      switch (n) {
      case 32512:
      case 32513:
      case 32514:
      case 32515:
      case 32642:
      case 32643:
      case 32644:
      case 32645:
      case 32646:
      case 32648:
      case 32649:
      case 32650:
        return (HANDLE)name;
      default:
        u32_fail(1814);
        return 0;
      }
    }
    DWORD resource = U32_PTR(name);
    const DWORD *pixels = resource == 32512   ? ciuki_icon32
                          : resource == 32513 ? tango_error
                          : resource == 32514 ? tango_question
                          : resource == 32515 ? tango_warning
                          : resource == 32516 ? tango_information
                                              : 0;
    if (!pixels) {
      u32_fail(1814);
      return 0;
    }
    for (UINT k = 0; k < 64; k++)
      if (icons[k].id && icons[k].shared && !icons[k].cursor &&
          icons[k].resource == resource)
        return (HANDLE)(uintptr_t)icons[k].id;
    struct icon *i = allocate(0);
    if (!i)
      return 0;
    i->shared = 1;
    i->resource = resource;
    for (UINT p = 0; p < 1024; p++) {
      i->pixels[p] = pixels[p];
      i->mask[p] = (pixels[p] >> 24) == 0;
    }
    return (HANDLE)(uintptr_t)i->id;
  }
  HANDLE group =
      FindResourceA(instance, name, (void *)(uintptr_t)(cursor ? 12 : 14));
  if (!group)
    return 0;
  const uint8_t *g = LoadResource(instance, group);
  DWORD gn = SizeofResource(instance, group);
  if (!g || gn < 6 || w32_u16(g + 2) != (cursor ? 2 : 1) || !w32_u16(g + 4) ||
      gn < 6u + 14u * w32_u16(g + 4)) {
    u32_fail(13);
    return 0;
  }
  UINT best = 0;
  if (cursor) {
    for (UINT k = 0; k < w32_u16(g + 4); k++)
      if (w32_u16(g + 6 + k * 14) == 32) {
        best = k;
        break;
      }
  } else {
    /* fx-exeicons: the 32x32 image Windows picks (Hornick, "Choosing an
     * Icon"): the closest size, then the greatest colour depth. */
    unsigned order[1];
    if (iconx_rank(g, gn, 14, 32, order, 1) == 1)
      best = order[0];
  }
  UINT rid = w32_u16(g + 6 + best * 14 + 12);
  HANDLE resource = FindResourceA(instance, (void *)(uintptr_t)rid,
                                  (void *)(uintptr_t)(cursor ? 1 : 3));
  if (!resource)
    return 0;
  const uint8_t *b = LoadResource(instance, resource);
  DWORD n = SizeofResource(instance, resource);
  struct icon *i = from_bits(b, n, cursor);
  if (!i)
    return 0;
  i->shared = 1;
  if (!cursor)
    group_small(instance, g, gn, i);
  return (HANDLE)(uintptr_t)i->id;
}
HCURSOR W32API LoadCursorA(HINSTANCE i, const char *name) {
  U32_GUARD;
  return load(i, name, 1);
}
HICON W32API LoadIconA(HINSTANCE i, const char *name) {
  U32_GUARD;
  return load(i, name, 0);
}
HICON W32API CreateIconIndirect(const ICONINFO *info) {
  U32_GUARD;
  if (!info || !u32_gdi || !u32_gdi->bitmap) {
    u32_fail(120);
    return 0;
  }
  struct u32_surface color = {0}, mask = {0};
  if (info->hbmColor && !u32_gdi->bitmap(info->hbmColor, &color, 0))
    return 0;
  if (info->hbmMask && !u32_gdi->bitmap(info->hbmMask, &mask, 0)) {
    u32_gdi->bitmap(info->hbmColor, &color, 1);
    return 0;
  }
  if (color.width != 32 || color.height != 32 || color.format != 1 ||
      (mask.pixels && (mask.width != 32 || mask.height < 32)) ||
      (!info->fIcon && (info->xHotspot >= 32 || info->yHotspot >= 32))) {
    u32_gdi->bitmap(info->hbmColor, &color, 1);
    u32_gdi->bitmap(info->hbmMask, &mask, 1);
    u32_fail(120);
    return 0;
  }
  struct icon *i = allocate(!info->fIcon);
  if (!i) {
    u32_gdi->bitmap(info->hbmColor, &color, 1);
    u32_gdi->bitmap(info->hbmMask, &mask, 1);
    return 0;
  }
  i->hotx = info->xHotspot;
  i->hoty = info->yHotspot;
  for (UINT y = 0; y < 32; y++)
    for (UINT x = 0; x < 32; x++) {
      UINT p = y * 32 + x;
      i->pixels[p] =
          w32_u32((uint8_t *)color.pixels + y * color.pitch + x * 4) |
          0xff000000;
      if (mask.pixels) {
        BOOL and = (w32_u32((uint8_t *)mask.pixels + y * mask.pitch + x * 4) &
                    0xffffff) != 0;
        i->mask[p] = and;
      }
    }
  u32_gdi->bitmap(info->hbmColor, &color, 1);
  u32_gdi->bitmap(info->hbmMask, &mask, 1);
  return (HICON)(uintptr_t)i->id;
}
/* fx-hlrun (MFC and OLEAUT32 import it): an icon from raw AND-mask and XOR
 * bits, as Windows does it: a monochrome mask bitmap and a color bitmap of
 * planes x bitsPixel are made from the bits and passed to CreateIconIndirect
 * with a centered hot spot (Wine 10.0 dlls/user32/cursoricon.c CreateIcon).
 * USER's icon store holds 32x32 XRGB pixels and a mask, so both bitmaps are
 * first converted to 32 bits per pixel by BitBlt (a monochrome bit is the
 * background color white when set, black when clear). */
static DWORD icon_plane(const struct u32_graphics *g, int w, int h, UINT planes, UINT bpp, const void *bits) {
  DWORD source = g->create_bitmap(w, h, planes, bpp, bits), out = g->create_bitmap(w, h, 1, 32, 0);
  DWORD sdc = g->memory_dc(0), ddc = g->memory_dc(0);
  BOOL ok = source && out && sdc && ddc;
  if (ok) {
    DWORD a = g->select(sdc, source), b = g->select(ddc, out);
    ok = g->bitblt(ddc, 0, 0, w, h, sdc, 0, 0, 0x00cc0020u /* SRCCOPY */);
    g->select(sdc, a);
    g->select(ddc, b);
  }
  if (sdc)
    g->delete_dc(sdc);
  if (ddc)
    g->delete_dc(ddc);
  if (source)
    g->delete_object(source);
  if (!ok && out) {
    g->delete_object(out);
    out = 0;
  }
  return out;
}
HICON W32API CreateIcon(HINSTANCE instance, int w, int h, uint8_t planes, uint8_t bpp, const uint8_t *and_bits,
                        const uint8_t *xor_bits) {
  U32_GUARD;
  (void)instance;
  const struct u32_graphics *g = u32_graphics();
  if (!and_bits || !xor_bits || w <= 0 || h <= 0 || !planes || !bpp) {
    u32_fail(87);
    return 0;
  }
  if (!g || !g->create_bitmap || !g->memory_dc || !g->select || !g->bitblt || !g->delete_dc ||
      !g->delete_object) {
    u32_fail(120);
    return 0;
  }
  DWORD mask = icon_plane(g, w, h, 1, 1, and_bits), color = icon_plane(g, w, h, planes, bpp, xor_bits);
  HICON icon = 0;
  if (mask && color) {
    ICONINFO info = {1, (DWORD)w / 2, (DWORD)h / 2, (HANDLE)(uintptr_t)mask, (HANDLE)(uintptr_t)color};
    icon = CreateIconIndirect(&info);
  } else
    u32_fail(8);
  if (mask)
    g->delete_object(mask);
  if (color)
    g->delete_object(color);
  return icon;
}
BOOL W32API DrawIconEx(HDC dc, int x, int y, HICON handle, int w, int h,
                       UINT frame, HBRUSH brush, UINT flags) {
  U32_GUARD;
  u32_gdi_ensure();
  if (frame)
    return u32_fail(87);
  struct icon *i = find(handle);
  if (!i || !u32_gdi || !u32_gdi->icon)
    return u32_fail(120);
  if (w < 0 || h < 0 || !(flags & 3) || (flags & ~0xf)) return u32_fail(87);
  RECT area = {x, y, x + (w ? w : 32), y + (h ? h : 32)};
  if (brush && (!u32_gdi->fill || !u32_gdi->fill(dc, &area, brush))) return 0;
  return u32_gdi->icon(dc, x, y, w ? w : 32, h ? h : 32, i->pixels, i->mask, 32,
                       flags);
}
BOOL W32API DrawIcon(HDC dc, int x, int y, HICON h) {
  U32_GUARD;
  return DrawIconEx(dc, x, y, h, 32, 32, 0, 0, 3);
}

/* ---- f5-20B: icon copies, ICONINFO, resource images, extraction ------- */
HICON u32_icon_copy(HICON h) {
  struct icon *src = find(h);
  if (!src)
    return (HICON)(uintptr_t)u32_fail(1402);
  /* allocate selects an unused slot; src has a live ID, so cannot alias it.
   * Copy directly instead of putting a 5 KiB icon on the UI thread stack. */
  struct icon *i = allocate(src->cursor);
  if (!i)
    return 0;
  DWORD id = i->id;
  *i = *src;
  i->id = id;
  i->shared = 0;
  return (HICON)(uintptr_t)i->id;
}
HICON W32API CopyIcon(HICON h) {
  U32_GUARD;
  return u32_icon_copy(h);
}
BOOL u32_is_icon(HANDLE h, BOOL *cursor) {
  struct icon *i = find(h);
  if (i && cursor)
    *cursor = i->cursor;
  return i != 0;
}
/* GetIconInfo: new bitmaps the caller deletes, a 1-bpp AND mask and a
 * 32-bpp colour image (black where the mask is set); the icon hotspot is
 * its centre. */
BOOL u32_icon_info(HICON h, ICONINFO *out) {
  struct icon *i = find(h);
  const struct u32_graphics *g = u32_graphics();
  if (!i)
    return u32_fail(1402);
  if (!out)
    return u32_fail(87);
  if (!g || !g->create_bitmap)
    return u32_fail(120);
  uint8_t mask[32 * 4];
  w32_fill(mask, 0, sizeof(mask));
  for (UINT p = 0; p < 1024; p++)
    if (i->mask[p])
      mask[(p / 32) * 4 + (p % 32) / 8] |= (uint8_t)(128 >> (p & 7));
  DWORD m = g->create_bitmap(32, 32, 1, 1, mask);
  DWORD c = m ? g->create_bitmap(32, 32, 1, 32, i->pixels) : 0;
  if (!c) {
    if (m)
      g->delete_object(m);
    return u32_fail(8);
  }
  out->fIcon = !i->cursor;
  out->xHotspot = i->cursor ? i->hotx : 16;
  out->yHotspot = i->cursor ? i->hoty : 16;
  out->hbmMask = (HANDLE)(uintptr_t)m;
  out->hbmColor = (HANDLE)(uintptr_t)c;
  return 1;
}
BOOL W32API GetIconInfo(HICON h, ICONINFO *out) {
  U32_GUARD;
  return u32_icon_info(h, out);
}
HICON u32_icon_from_resource(const uint8_t *bits, DWORD size, BOOL icon,
                             BOOL shared) {
  struct icon *i = from_bits(bits, size, !icon);
  if (!i)
    return 0;
  i->shared = shared;
  return (HICON)(uintptr_t)i->id;
}
/* Version 0x00030000 (Win32) images; LR_SHARED (0x8000) makes the handle
 * survive DestroyIcon. Requested sizes are met by resampling to the 32x32
 * USER icon. */
HICON W32API CreateIconFromResourceEx(const uint8_t *bits, DWORD size,
                                      BOOL icon, DWORD version, int cx, int cy,
                                      UINT flags) {
  U32_GUARD;
  (void)cx, (void)cy;
  if (version != 0x00030000u && version != 0x00020000u)
    return (HICON)(uintptr_t)u32_fail(87);
  return u32_icon_from_resource(bits, size, icon, !!(flags & 0x8000u));
}
HICON W32API CreateIconFromResource(const uint8_t *bits, DWORD size, BOOL icon,
                                    DWORD version) {
  return CreateIconFromResourceEx(bits, size, icon, version, 0, 0, 0);
}
/* CopyImage: bitmaps are copied through two memory DCs (stretched to cx/cy
 * when given); icons and cursors are duplicated (USER icons are 32x32).
 * LR_COPYDELETEORG (8) deletes the original. */
HANDLE W32API CopyImage(HANDLE h, UINT type, int cx, int cy, UINT flags) {
  U32_GUARD;
  const struct u32_graphics *g = u32_graphics();
  if (type == 1 || type == 2) {
    HICON copy = u32_icon_copy(h);
    if (copy && (flags & 8)) {
      struct icon *i = find(h);
      if (i && !i->shared)
        w32_fill(i, 0, sizeof(*i));
    }
    return copy;
  }
  if (type != 0)
    return (HANDLE)(uintptr_t)u32_fail(87);
  struct {
    LONG type, width, height, pitch;
    uint16_t planes, bpp;
    void *bits;
  } bm;
  if (!g || !g->object || !g->memory_dc || !g->create_bitmap || !g->bitblt ||
      g->object(U32_PTR(h), sizeof(bm), &bm) <= 0)
    return (HANDLE)(uintptr_t)u32_fail(6);
  int w = cx > 0 ? cx : bm.width, height = cy > 0 ? cy : bm.height;
  DWORD from = g->memory_dc(0), to = g->memory_dc(0);
  DWORD copy = from && to ? g->create_bitmap(w, height, 1, bm.bpp, 0) : 0;
  BOOL ok = 0;
  if (copy) {
    DWORD old_from = g->select(from, U32_PTR(h)), old_to = g->select(to, copy);
    BOOL(W32API * stretch)(DWORD, int, int, int, int, DWORD, int, int, int,
                            int, DWORD) =
        (BOOL(W32API *)(DWORD, int, int, int, int, DWORD, int, int, int, int,
                        DWORD))GetProcAddress(GetModuleHandleA("gdi32.dll"),
                                              "StretchBlt");
    ok = w == bm.width && height == bm.height
             ? g->bitblt(to, 0, 0, w, height, from, 0, 0, 0xcc0020u)
             : stretch && stretch(to, 0, 0, w, height, from, 0, 0, bm.width,
                                  bm.height, 0xcc0020u);
    g->select(from, old_from);
    g->select(to, old_to);
  }
  if (from)
    g->delete_dc(from);
  if (to)
    g->delete_dc(to);
  if (!ok) {
    if (copy)
      g->delete_object(copy);
    return (HANDLE)(uintptr_t)u32_fail(8);
  }
  if (flags & 8)
    g->delete_object(U32_PTR(h));
  return (HANDLE)(uintptr_t)copy;
}

/* PrivateExtractIcons: icon groups of a PE image (loaded as a data file)
 * or the image of a .ICO file. index >= 0 counts groups in resource order;
 * a negative index names the group's resource identifier. */
struct extract {
  UINT wanted, seen, count;
  int index;
  HICON *icons;
  UINT *ids;
  HANDLE module;
};
static HICON group_icon(HANDLE module, const void *name) {
  HANDLE group = FindResourceA(module, name, (void *)(uintptr_t)14);
  const uint8_t *g = group ? LoadResource(module, group) : 0;
  DWORD gn = group ? SizeofResource(module, group) : 0;
  if (!g || gn < 6 || w32_u16(g + 2) != 1 || !w32_u16(g + 4) ||
      gn < 6u + 14u * w32_u16(g + 4))
    return 0;
  /* fx-exeicons: the closest size, then the deepest image (iconx_rank),
   * with the group's own 16x16 image for the small icon. */
  unsigned order[1];
  UINT best = iconx_rank(g, gn, 14, 32, order, 1) == 1 ? order[0] : 0;
  UINT rid = w32_u16(g + 6 + best * 14 + 12);
  HANDLE r = FindResourceA(module, (void *)(uintptr_t)rid, (void *)(uintptr_t)3);
  const uint8_t *b = r ? LoadResource(module, r) : 0;
  HICON icon = b ? u32_icon_from_resource(b, SizeofResource(module, r), 1, 0) : 0;
  struct icon *i = icon ? find(icon) : 0;
  if (i)
    group_small(module, g, gn, i);
  return icon;
}
static BOOL W32API group_found(HANDLE module, const char *type, char *name,
                               intptr_t context) {
  (void)type;
  struct extract *x = (struct extract *)context;
  UINT at = x->seen++;
  BOOL take = x->index >= 0 ? at >= (UINT)x->index
                            : (uintptr_t)name == (uintptr_t)(UINT)-x->index;
  if (!take)
    return 1;
  if (x->icons) {
    HICON icon = group_icon(module, name);
    if (icon) {
      x->icons[x->count] = icon;
      if (x->ids)
        x->ids[x->count] = U32_ATOM(name) ? (UINT)(uintptr_t)name : 0;
      x->count++;
    }
  } else
    x->count++;
  return x->count < x->wanted;
}
/* fx-exeicons: an icon file (.ico, .cur, .ani, .bmp) or a 16-bit NE
 * program or library, read whole (bounded) and extracted by iconx with the
 * ExtractIcon index rules. 1 handled (*result set), 0 not such a file (a PE
 * image goes through the loader's resource tree below), the file missing
 * reports UINT_MAX as ExtractIconEx does. */
#define U32_ICON_FILE_MAX (8u << 20)
static int file_icons(const char *path, int index, HICON *icons, UINT *ids, UINT wanted, UINT *result) {
  HANDLE f = CreateFileA(path, 0x80000000u, 1, 0, 3, 0, 0);
  if (!f || f == (HANDLE)(intptr_t)-1)
    return 0;
  DWORD size = GetFileSize(f, 0), head = size < 4096 ? size : 4096, got = 0;
  uint8_t *data = u32_alloc(head ? head : 1);
  int handled = 0;
  if (data && size != 0xffffffffu && ReadFile(f, data, head, &got, 0) && got == head) {
    struct iconx_memory m = {data, head};
    struct iconx *x = u32_alloc(sizeof(*x));
    if (x) {
      iconx_init(x, iconx_memory_read, &m, head);
      int kind = iconx_kind(x);
      if (kind == ICONX_DOS && head >= 64 && w32_u32(data + 60) + 4 > head && size > head)
        kind = ICONX_PE;   /* a header past the probe: the loader decides */
      if (kind == ICONX_DOS) { handled = 1; *result = 0; }
      else if (kind != ICONX_PE && kind != ICONX_UNKNOWN && size <= U32_ICON_FILE_MAX) {
        uint8_t *whole = size > head ? u32_alloc(size) : data;
        DWORD more = 0;
        if (whole && whole != data) {
          w32_copy(whole, data, head);
          if (!ReadFile(f, whole + head, size - head, &more, 0) || more != size - head) {
            u32_free(whole);
            whole = 0;
          }
        }
        if (whole) {
          m = (struct iconx_memory){whole, size};
          iconx_init(x, iconx_memory_read, &m, size);
          int count = iconx_count(x);
          handled = 1;
          *result = 0;
          if (!icons)
            *result = count > 0 ? (UINT)count : 0;
          else {
            uint32_t *argb = u32_alloc((1024 + 256) * 4);
            for (UINT k = 0; argb && k < wanted; k++) {
              if (index < 0 && k)
                break;
              int at = index >= 0 ? index + (int)k : index;
              if (iconx_extract(x, at, 32, argb))
                break;
              struct icon *i = allocate(0);
              if (!i)
                break;
              icon_set_argb(i, argb);
              if (!iconx_extract(x, at, 16, argb + 1024)) {
                w32_copy(i->small, argb + 1024, sizeof(i->small));
                i->has_small = 1;
              }
              icons[*result] = (HICON)(uintptr_t)i->id;
              if (ids)
                ids[*result] = x->group_id;
              (*result)++;
            }
            if (argb)
              u32_free(argb);
          }
          if (whole != data)
            u32_free(whole);
        }
      }
      u32_free(x);
    }
  }
  if (data)
    u32_free(data);
  CloseHandle(f);
  return handled;
}
static UINT extract(const char *path, int index, HICON *icons, UINT *ids,
                    UINT wanted) {
  if (!path)
    return (UINT)u32_fail(87);
  UINT handled;
  if (file_icons(path, index, icons, ids, icons ? wanted : 0, &handled))
    return handled;
  HANDLE(W32API * load)(const char *, HANDLE, DWORD) =
      (HANDLE(W32API *)(const char *, HANDLE, DWORD))u32_kernel("LoadLibraryExA");
  BOOL(W32API * unload)(HANDLE) = (BOOL(W32API *)(HANDLE))u32_kernel("FreeLibrary");
  BOOL(W32API * names)(HANDLE, const char *,
                       BOOL(W32API *)(HANDLE, const char *, char *, intptr_t),
                       intptr_t) =
      (BOOL(W32API *)(HANDLE, const char *,
                      BOOL(W32API *)(HANDLE, const char *, char *, intptr_t),
                      intptr_t))u32_kernel("EnumResourceNamesA");
  if (!load || !names)
    return (UINT)u32_fail(120);
  HANDLE module = load(path, 0, 2); /* LOAD_LIBRARY_AS_DATAFILE */
  if (!module)
    return icons ? 0xffffffffu : 0u;
  struct extract x = {icons ? wanted : 0xffffffffu, 0, 0, index, icons, ids,
                      module};
  if (!icons)
    x.index = 0;
  if (!icons || wanted)
    names(module, (const char *)(uintptr_t)14, group_found, (intptr_t)&x);
  if (unload)
    unload(module);
  return x.count;
}
UINT W32API PrivateExtractIconsA(const char *path, int index, int cx, int cy,
                                 HICON *icons, UINT *ids, UINT n,
                                 UINT flags) {
  U32_GUARD;
  (void)cx, (void)cy, (void)flags;
  return extract(path, index, icons, ids, icons ? n : 0);
}
UINT W32API PrivateExtractIconsW(const WCHAR *path, int index, int cx, int cy,
                                 HICON *icons, UINT *ids, UINT n,
                                 UINT flags) {
  U32_GUARD;
  (void)cx, (void)cy, (void)flags;
  char *a = path ? u32_narrow(path) : 0;
  if (!a)
    return 0;
  UINT r = extract(a, index, icons, ids, icons ? n : 0);
  u32_text_free(a);
  return r;
}
/* PrivateExtractIconEx: large and small icons (both the 32x32 USER icon);
 * index -1 with no outputs counts the icons. */
UINT W32API PrivateExtractIconExA(const char *path, int index, HICON *large,
                                  HICON *small, UINT n) {
  U32_GUARD;
  if (index == -1 && !large && !small)
    return extract(path, 0, 0, 0, 0);
  UINT got = 0;
  if (large)
    got = extract(path, index, large, 0, n);
  if (small) {
    UINT s = extract(path, index, small, 0, n);
    if (!large)
      got = s;
  }
  return got == 0xffffffffu ? 0 : got;
}
UINT W32API PrivateExtractIconExW(const WCHAR *path, int index, HICON *large,
                                  HICON *small, UINT n) {
  U32_GUARD;
  char *a = path ? u32_narrow(path) : 0;
  if (!a)
    return 0;
  UINT r = PrivateExtractIconExA(a, index, large, small, n);
  u32_text_free(a);
  return r;
}

/* ---- fx-exeicons: the icon a window shows on the desktop ------------------
 * Microsoft: WM_GETICON ("A window that has no icon explicitly set (with
 * WM_SETICON) uses the icon for the registered window class"), WNDCLASSEX
 * hIconSm ("If this member is NULL, the system searches the icon resource
 * specified by the hIcon member for an icon of the appropriate size").
 * A window with neither shows its executable's first icon group (the owner's
 * fx-exeicons requirement; Windows documents a default icon there). The
 * desktop draws the 16 px icon in the caption and on the taskbar button and
 * the 32 px one in Alt+Tab. Only captioned top-level windows have them. */
static HICON exe_icon(void) {
  static HICON cached;
  static BOOL tried;
  if (tried)
    return cached;
  tried = 1;
  BOOL(W32API * names)(HANDLE, const char *,
                       BOOL(W32API *)(HANDLE, const char *, char *, intptr_t),
                       intptr_t) =
      (BOOL(W32API *)(HANDLE, const char *,
                      BOOL(W32API *)(HANDLE, const char *, char *, intptr_t),
                      intptr_t))u32_kernel("EnumResourceNamesA");
  HANDLE module = GetModuleHandleA(0);
  HICON icon = 0;
  struct extract x = {1, 0, 0, 0, &icon, 0, module};
  if (names && module)
    names(module, (const char *)(uintptr_t)14, group_found, (intptr_t)&x);
  struct icon *i = x.count ? find(icon) : 0;
  if (i)
    i->shared = 1;
  return cached = i ? icon : 0;
}
static void icon_argb(const struct icon *i, uint32_t *out32, uint32_t *out16) {
  if (out32)
    for (UINT p = 0; p < 1024; p++)
      out32[p] = i->mask[p] ? 0 : i->pixels[p] | 0xff000000u;
  if (!out16)
    return;
  if (i->has_small) {
    w32_copy(out16, i->small, sizeof(i->small));
    return;
  }
  for (UINT y = 0; y < 16; y++)
    for (UINT x = 0; x < 16; x++) {
      UINT a = 0, r = 0, g = 0, b = 0;
      for (UINT k = 0; k < 4; k++) {
        UINT p = (2 * y + k / 2) * 32 + 2 * x + k % 2;
        if (i->mask[p])
          continue;
        a += 255; r += (i->pixels[p] >> 16 & 255) * 255;
        g += (i->pixels[p] >> 8 & 255) * 255; b += (i->pixels[p] & 255) * 255;
      }
      out16[y * 16 + x] = a ? ((a + 2) / 4) << 24 | ((r + a / 2) / a) << 16 |
                                  ((g + a / 2) / a) << 8 | (b + a / 2) / a
                            : 0;
    }
}
static BOOL send_icon(struct u32_window *w, UINT size, const uint32_t *argb) {
  struct u32_surface s = {.fd = -1};
  if (!u32_backend.surface(size, size, &s))
    return 0;
  for (UINT y = 0; y < size; y++)
    w32_copy((uint8_t *)s.pixels + y * s.pitch, argb + y * size, size * 4);
  uint8_t b[4];
  desk_put32(b, size);
  BOOL ok = u32_wire(w, DESK_V3_WINDOW_ICON, b, 4, s.fd);
  u32_backend.drop_surface(&s);
  return ok;
}
BOOL u32_window_icon(struct u32_window *w) {
  if (!w || !w->wire || w->parent || (w->exstyle & WS_EX_TOOLWINDOW) ||
      ((w->style & WS_CAPTION) != WS_CAPTION && !(w->exstyle & 0x40000u /* WS_EX_APPWINDOW */)))
    return 1;
  struct icon *big = find(w->icon_big ? w->icon_big : w->cls ? w->cls->c.hIcon : 0);
  struct icon *small = find(w->icon_small ? w->icon_small : w->cls ? w->cls->c.hIconSm : 0);
  if (big && big->cursor)
    big = 0;
  if (small && small->cursor)
    small = 0;
  if (!big && !small)
    big = find(exe_icon());
  if (!big && !small)
    return 1;   /* the desktop's default */
  uint32_t *argb = u32_alloc((1024 + 256) * 4);
  if (!argb)
    return 0;
  icon_argb(big ? big : small, argb, 0);
  icon_argb(small ? small : big, 0, argb + 1024);
  BOOL ok = send_icon(w, 32, argb) && send_icon(w, 16, argb + 1024);
  u32_free(argb);
  return ok;
}

/* fx-exeicons: the shell's default icons for ciukishell's SHGetFileInfo, the
 * Tango 0.8.90 images the native shell shows for the same objects: 0 a file
 * without its own icon, 1 a folder, 2 a program without icons, 3 an open
 * folder. Shared handles (DestroyIcon keeps them), with their 16x16 image. */
HICON W32API CiukiShellIcon(UINT kind) {
  U32_GUARD;
  static const unsigned int *const large[4] = {tango_document32, tango_folder32, tango_program32, tango_folder_open32};
  static const unsigned int *const little[4] = {tango_document16, tango_folder16, tango_program16, tango_folder_open16};
  if (kind > 3)
    return (HICON)(uintptr_t)u32_fail(87);
  DWORD resource = 0x10000u + kind;
  for (UINT k = 0; k < 64; k++)
    if (icons[k].id && icons[k].shared && !icons[k].cursor && icons[k].resource == resource)
      return (HICON)(uintptr_t)icons[k].id;
  struct icon *i = allocate(0);
  if (!i)
    return 0;
  i->shared = 1;
  i->resource = resource;
  icon_set_argb(i, large[kind]);
  w32_copy(i->small, little[kind], sizeof(i->small));
  i->has_small = 1;
  return (HICON)(uintptr_t)i->id;
}
