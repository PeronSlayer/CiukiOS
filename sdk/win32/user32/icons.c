/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "ciuki-icon32.h"
#include "unicode.h"
struct icon {
  DWORD id, resource;
  BOOL cursor, shared;
  UINT hotx, hoty;
  DWORD pixels[1024];
  uint8_t mask[1024];
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
  if (show && cursor_count < 32767)
    cursor_count++;
  if (!show && cursor_count > -32768)
    cursor_count--;
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
 * mask; cursors start with the hotspot) of any square size up to 256
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
  if (width <= 0 || width > 256 || height2 != 2 * width ||
      (depth != 1 && depth != 4 && depth != 8 && depth != 24 && depth != 32))
    return (struct icon *)(uintptr_t)u32_fail(13);
  UINT size = (UINT)width;
  UINT colors =
      depth <= 8 ? (w32_u32(b + 32) ? w32_u32(b + 32) : 1u << depth) : 0;
  if (colors > 256)
    return (struct icon *)(uintptr_t)u32_fail(13);
  UINT pitch = ((size * depth + 31) / 32) * 4, offset = 40 + colors * 4,
       mask_pitch = ((size + 31) / 32) * 4;
  if ((uint64_t)offset + (uint64_t)pitch * size + (uint64_t)mask_pitch * size >
      n)
    return (struct icon *)(uintptr_t)u32_fail(13);
  struct icon *i = allocate(cursor);
  if (!i)
    return 0;
  i->hotx = size == 32 ? hotx : hotx * 32 / size;
  i->hoty = size == 32 ? hoty : hoty * 32 / size;
  for (UINT y = 0; y < 32; y++)
    for (UINT x = 0; x < 32; x++) {
      UINT sx = x * size / 32, sy = y * size / 32;
      const uint8_t *row = b + offset + (size - 1 - sy) * pitch;
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
      BOOL mask = !!(b[offset + pitch * size + (size - 1 - sy) * mask_pitch +
                       sx / 8] &
                     (128 >> (sx & 7)));
      i->mask[p] = mask;
      i->pixels[p] = mask ? 0 : (c & 0xffffff) | 0xff000000;
    }
  return i;
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
  for (UINT k = 0; k < w32_u16(g + 4); k++) {
    const uint8_t *entry = g + 6 + k * 14;
    if ((cursor ? w32_u16(entry) : entry[0]) == 32) {
      best = k;
      break;
    }
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
  UINT best = 0, best_score = 0xffffffffu;
  for (UINT k = 0; k < w32_u16(g + 4); k++) {
    const uint8_t *e = g + 6 + k * 14;
    UINT size = e[0] ? e[0] : 256, score = size > 32 ? size - 32 : 32 - size;
    if (score < best_score) {
      best = k;
      best_score = score;
    }
  }
  UINT rid = w32_u16(g + 6 + best * 14 + 12);
  HANDLE r = FindResourceA(module, (void *)(uintptr_t)rid, (void *)(uintptr_t)3);
  const uint8_t *b = r ? LoadResource(module, r) : 0;
  return b ? u32_icon_from_resource(b, SizeofResource(module, r), 1, 0) : 0;
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
static HICON ico_file(const char *path, UINT *found) {
  HANDLE f = CreateFileA(path, 0x80000000u, 1, 0, 3, 0, 0);
  *found = 0;
  if (!f || f == (HANDLE)(intptr_t)-1)
    return 0;
  uint8_t *data = u32_alloc(1u << 18);
  DWORD n = 0;
  BOOL ok = data && ReadFile(f, data, 1u << 18, &n, 0);
  CloseHandle(f);
  *found = 1;
  HICON icon = 0;
  if (ok && n >= 6 + 16 && w32_u16(data) == 0 && w32_u16(data + 2) == 1 &&
      w32_u16(data + 4) && n >= 6u + 16u * w32_u16(data + 4)) {
    UINT best = 0, best_score = 0xffffffffu;
    for (UINT k = 0; k < w32_u16(data + 4); k++) {
      const uint8_t *e = data + 6 + k * 16;
      UINT size = e[0] ? e[0] : 256, score = size > 32 ? size - 32 : 32 - size;
      if (score < best_score) {
        best = k;
        best_score = score;
      }
    }
    const uint8_t *e = data + 6 + best * 16;
    DWORD bytes = w32_u32(e + 8), offset = w32_u32(e + 12);
    if (offset <= n && bytes <= n - offset)
      icon = u32_icon_from_resource(data + offset, bytes, 1, 0);
  }
  u32_free(data);
  return icon;
}
static UINT extract(const char *path, int index, HICON *icons, UINT *ids,
                    UINT wanted) {
  UINT n = path ? w32_strlen(path) : 0;
  if (!path)
    return (UINT)u32_fail(87);
  if (n > 4 && !u32_stricmp(path + n - 4, ".ico")) {
    UINT found;
    if (!icons)
      return ico_file(path, &found) ? 1u : 0u;
    if (index > 0 || !wanted)
      return 0;
    HICON icon = ico_file(path, &found);
    if (!found)
      return 0xffffffffu;
    if (!icon)
      return 0;
    icons[0] = icon;
    if (ids)
      ids[0] = 0;
    return 1;
  }
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
