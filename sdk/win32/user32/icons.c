/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "ciuki-icon32.h"
#include "internal.h"
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
static struct icon *find(HANDLE h) {
  for (UINT i = 0; i < 64; i++)
    if (icons[i].id == U32_PTR(h))
      return &icons[i];
  return 0;
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
  UINT hotx = 0, hoty = 0;
  if (cursor) {
    if (n < 4)
      return 0;
    hotx = w32_u16(b);
    hoty = w32_u16(b + 2);
    b += 4;
    n -= 4;
  }
  if (n < 40 || w32_u32(b) != 40 || w32_u32(b + 4) != 32 ||
      w32_u32(b + 8) != 64 || w32_u16(b + 12) != 1 || w32_u32(b + 16)) {
    u32_fail(120);
    return 0;
  }
  UINT depth = w32_u16(b + 14);
  if (depth != 1 && depth != 4 && depth != 8 && depth != 24 && depth != 32) {
    u32_fail(120);
    return 0;
  }
  UINT colors =
      depth <= 8 ? (w32_u32(b + 32) ? w32_u32(b + 32) : 1u << depth) : 0;
  if (colors > 256)
    return 0;
  UINT pitch = ((32 * depth + 31) / 32) * 4, offset = 40 + colors * 4;
  if ((uint64_t)offset + pitch * 32 + 128 > n)
    return (HANDLE)(uintptr_t)u32_fail(13);
  struct icon *i = allocate(cursor);
  if (!i)
    return 0;
  i->hotx = hotx;
  i->hoty = hoty;
  for (UINT y = 0; y < 32; y++)
    for (UINT x = 0; x < 32; x++) {
      const uint8_t *row = b + offset + (31 - y) * pitch;
      UINT p = y * 32 + x;
      DWORD c = 0;
      if (depth == 32)
        c = w32_u32(row + x * 4);
      else if (depth == 24)
        c = row[x * 3] | (DWORD)row[x * 3 + 1] << 8 |
            (DWORD)row[x * 3 + 2] << 16;
      else {
        UINT idx = depth == 8   ? row[x]
                   : depth == 4 ? (row[x / 2] >> (x & 1 ? 0 : 4)) & 15
                                : (row[x / 8] >> (7 - (x & 7))) & 1;
        if (idx >= colors) {
          i->id = 0;
          return (HANDLE)(uintptr_t)u32_fail(13);
        }
        c = w32_u32(b + 40 + idx * 4);
      }
      BOOL mask =
          !!(b[offset + pitch * 32 + (31 - y) * 4 + x / 8] & (128 >> (x & 7)));
      i->mask[p] = mask;
      i->pixels[p] = mask ? 0 : c | 0xff000000;
    }
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
