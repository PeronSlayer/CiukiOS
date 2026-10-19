/* Windows static colours and logical palette realisation.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
static DWORD system_colors[256];
static HPALETTE occupied[256];
static int system_ready;
static void (*notify)(void *, HDC, unsigned);
static void *notify_opaque;
static HPALETTE foreground;
static DWORD epoch = 1;
void gdi_palette_reset(void) {
  system_ready = 0;
  foreground = 0;
  epoch++;
  notify = 0;
  notify_opaque = 0;
  w32_fill(occupied, 0, sizeof(occupied));
}
void gdi_system_palette(DWORD *out) {
  if (!system_ready) {
    static const DWORD low[10] = {0,        0x800000, 0x008000, 0x808000,
                                  0x000080, 0x800080, 0x008080, 0xc0c0c0,
                                  0xc0dcc0, 0xa6caf0};
    static const DWORD high[10] = {0xfffbf0, 0xa0a0a0, 0x808080, 0xff0000,
                                   0x00ff00, 0xffff00, 0x0000ff, 0xff00ff,
                                   0x00ffff, 0xffffff};
    for (unsigned i = 0; i < 256; i++)
      system_colors[i] = (i << 16) | (i << 8) | i;
    for (unsigned i = 0; i < 10; i++) {
      system_colors[i] = low[i];
      system_colors[246 + i] = high[i];
      occupied[i] = occupied[246 + i] = 1;
    }
    system_ready = 1;
  }
  if (out)
    w32_copy(out, system_colors, sizeof(system_colors));
}
void W32API w32_gdi_palette_hook(void (*hook)(void *, HDC, unsigned),
                                 void *opaque) {
  GDI_GUARD;
  notify = hook;
  notify_opaque = opaque;
}
static DWORD color(PALETTEENTRY c) {
  return ((DWORD)c.peRed << 16) | ((DWORD)c.peGreen << 8) | c.peBlue;
}
static unsigned nearest(const DWORD *colors, unsigned n, DWORD c) {
  DWORD distance = 0xffffffff;
  unsigned index = 0;
  for (unsigned i = 0; i < n; i++) {
    int r = (int)(c >> 16) - (int)(colors[i] >> 16),
        g = (int)((c >> 8) & 255) - (int)((colors[i] >> 8) & 255),
        b = (int)(c & 255) - (int)(colors[i] & 255);
    DWORD d = (DWORD)(r * r + g * g + b * b);
    if (d < distance) {
      distance = d;
      index = i;
      if (!d)
        break;
    }
  }
  return index;
}
static int logical_index(struct dc *d, COLORREF c, struct palette **out) {
  struct object *o = gdi_object(d->s.palette, GDI_PALETTE);
  if (!o)
    return -1;
  *out = &o->u.palette;
  unsigned flags = c >> 24;
  if (flags == 1) {
    unsigned i = c & 65535;
    return i < o->u.palette.count ? (int)i : -1;
  }
  if (flags != 2)
    return -1;
  DWORD colors[256];
  for (unsigned i = 0; i < o->u.palette.count; i++)
    colors[i] = color(o->u.palette.entries[i]);
  return (int)nearest(colors, o->u.palette.count, gdi_color(c));
}
DWORD gdi_resolve_color(struct dc *d, COLORREF c) {
  unsigned flags = c >> 24;
  if (flags != 1 && flags != 2)
    return gdi_color(c);
  if (flags == 2 && gdi_device_bpp(d) > 8)
    return gdi_color(c);
  struct palette *p = 0;
  int index = logical_index(d, c, &p);
  return index >= 0 ? color(p->entries[index]) : 0;
}
DWORD gdi_encode_color(struct dc *d, COLORREF c) {
  struct w32_dib *s = gdi_target(d);
  unsigned flags = c >> 24;
  if (s->bpp <= 8 && (flags == 1 || (flags == 2 && gdi_device_bpp(d) <= 8))) {
    struct palette *p = 0;
    int index = logical_index(d, c, &p);
    struct object *bitmap = d->memory ? gdi_object(d->s.bitmap, GDI_BITMAP) : 0;
    if (index >= 0 && (!bitmap || !bitmap->u.bitmap.dib_section) &&
        p->realized_epoch == epoch && p->mapping[index] < s->color_count)
      return p->mapping[index];
  }
  return w32_dib_encode(s, gdi_resolve_color(d, c));
}
HPALETTE W32API CreatePalette(const LOGPALETTE *p) {
  GDI_GUARD;
  if (!p || p->palVersion != 0x300 || !p->palNumEntries ||
      p->palNumEntries > 256)
    return 0;
  HPALETTE h;
  struct object *o = gdi_new(GDI_PALETTE, &h);
  if (!o)
    return 0;
  o->u.palette.count = p->palNumEntries;
  o->u.palette.dirty = 1;
  w32_copy(o->u.palette.entries, p->palPalEntry,
           p->palNumEntries * sizeof(PALETTEENTRY));
  return h;
}
HPALETTE W32API SelectPalette(HDC h, HPALETTE palette, BOOL background) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d || !gdi_object(palette, GDI_PALETTE))
    return 0;
  HPALETTE old = d->s.palette;
  gdi_ref(palette, 1);
  gdi_ref(old, -1);
  d->s.palette = palette;
  d->s.palette_background = background != 0;
  return old;
}
unsigned W32API RealizePalette(HDC h) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d)
    return 0;
  struct object *o = gdi_object(d->s.palette, GDI_PALETTE);
  if (!o)
    return 0;
  struct palette *p = &o->u.palette;
  gdi_system_palette(0);
  int truecolor = GetDeviceCaps(h, 12) > 8;
  if (!d->s.palette_background && foreground != d->s.palette) {
    if (notify)
      notify(notify_opaque, h, 0x30f);
    for (unsigned i = 10; i < 246; i++)
      occupied[i] = 0;
    foreground = d->s.palette;
    epoch++;
  }
  if (p->realized_epoch == epoch && !p->dirty)
    return 0;
  for (unsigned i = 10; i < 246; i++)
    if (occupied[i] == d->s.palette)
      occupied[i] = 0;
  unsigned changed = 0;
  for (unsigned i = 0; i < p->count; i++) {
    PALETTEENTRY e = p->entries[i];
    DWORD c = color(e);
    unsigned index;
    if (e.peFlags & PC_EXPLICIT) {
      index = (unsigned)e.peRed | ((unsigned)e.peGreen << 8);
      index &= 255;
    } else {
      index = nearest(system_colors, 256, c);
      int match = 0;
      if (!(e.peFlags & (PC_RESERVED | PC_NOCOLLAPSE)))
        for (unsigned j = 0; j < 256; j++)
          if (occupied[j] && system_colors[j] == c) {
            index = j;
            match = 1;
            break;
          }
      if ((e.peFlags & (PC_RESERVED | PC_NOCOLLAPSE)) || !match) {
        unsigned j = 10;
        for (; j < 246; j++)
          if (!occupied[j])
            break;
        if (j < 246) {
          index = j;
          if (system_colors[index] != c && p->mapping[i] == index)
            changed++;
          system_colors[index] = c;
          occupied[index] = d->s.palette;
        }
      }
    }
    if (p->mapping[i] != index)
      changed++;
    p->mapping[i] = (uint8_t)index;
  }
  p->realized_epoch = epoch;
  p->dirty = 0;
  if (changed) {
    gdi_palette_targets();
    if (notify)
      notify(notify_opaque, h, 0x311);
  }
  return truecolor ? 0 : changed;
}
unsigned W32API SetPaletteEntries(HPALETTE h, unsigned first, unsigned count,
                                  const PALETTEENTRY *entries) {
  GDI_GUARD;
  struct object *o = gdi_object(h, GDI_PALETTE);
  if (!o || o->stock || !entries || first >= o->u.palette.count)
    return 0;
  if (count > o->u.palette.count - first)
    count = o->u.palette.count - first;
  w32_copy(o->u.palette.entries + first, entries, count * sizeof(*entries));
  o->u.palette.dirty = 1;
  return count;
}
unsigned W32API GetPaletteEntries(HPALETTE h, unsigned first, unsigned count,
                                  PALETTEENTRY *entries) {
  GDI_GUARD;
  struct object *o = gdi_object(h, GDI_PALETTE);
  if (!o)
    return 0;
  if (!entries)
    return o->u.palette.count;
  if (first >= o->u.palette.count)
    return 0;
  if (count > o->u.palette.count - first)
    count = o->u.palette.count - first;
  w32_copy(entries, o->u.palette.entries + first, count * sizeof(*entries));
  return count;
}
BOOL W32API AnimatePalette(HPALETTE h, unsigned first, unsigned count,
                           const PALETTEENTRY *entries) {
  GDI_GUARD;
  struct object *o = gdi_object(h, GDI_PALETTE);
  if (!o || o->stock || !entries || first >= o->u.palette.count)
    return 0;
  struct palette *p = &o->u.palette;
  if (count > p->count - first)
    count = p->count - first;
  gdi_system_palette(0);
  for (unsigned i = 0; i < count; i++)
    if (p->entries[first + i].peFlags & PC_RESERVED) {
      uint8_t flags = p->entries[first + i].peFlags;
      p->entries[first + i] = entries[i];
      p->entries[first + i].peFlags = flags;
      unsigned index = p->mapping[first + i];
      if (p->realized_epoch == epoch && index >= 10 && index < 246 &&
          occupied[index] == h)
        system_colors[index] = color(entries[i]);
    }
  gdi_palette_targets();
  if (notify)
    notify(notify_opaque, 0, 0x311);
  return 1;
}
unsigned W32API GetSystemPaletteEntries(HDC h, unsigned first, unsigned count,
                                        PALETTEENTRY *out) {
  GDI_GUARD;
  if (!gdi_dc(h))
    return 0;
  gdi_system_palette(0);
  if (!out)
    return 256;
  if (first >= 256)
    return 0;
  if (count > 256 - first)
    count = 256 - first;
  for (unsigned i = 0; i < count; i++) {
    DWORD c = system_colors[first + i];
    out[i] =
        (PALETTEENTRY){(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c, 0};
  }
  return count;
}
unsigned W32API GetNearestPaletteIndex(HPALETTE h, COLORREF c) {
  GDI_GUARD;
  struct object *o = gdi_object(h, GDI_PALETTE);
  if (!o)
    return CLR_INVALID;
  DWORD colors[256];
  for (unsigned i = 0; i < o->u.palette.count; i++)
    colors[i] = color(o->u.palette.entries[i]);
  return nearest(colors, o->u.palette.count, gdi_color(c));
}
COLORREF W32API GetNearestColor(HDC h, COLORREF c) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  struct w32_dib *s = gdi_target(d);
  if (!s)
    return CLR_INVALID;
  DWORD rgb = gdi_resolve_color(d, c);
  if (s->bpp <= 8)
    return gdi_colorref(s->colors[gdi_encode_color(d, c)]);
  uint8_t pixels[4] = {0};
  struct w32_dib temp = *s;
  temp.bits = pixels;
  temp.width = temp.height = 1;
  temp.pitch = temp.size = s->bpp / 8;
  temp.top_down = 1;
  w32_dib_put(&temp, 0, 0, w32_dib_encode(&temp, rgb));
  return gdi_colorref(w32_dib_rgb(&temp, 0, 0));
}
BOOL W32API UnrealizeObject(HGDIOBJ h) {
  GDI_GUARD;
  struct object *o = gdi_object(h, 0);
  if (!o)
    return 0;
  if (foreground == h)
    foreground = 0;
  for (unsigned i = 10; i < 246; i++)
    if (occupied[i] == h)
      occupied[i] = 0;
  if (o->type == GDI_PALETTE)
    o->u.palette.dirty = 1;
  return 1;
}
unsigned W32API SetDIBColorTable(HDC h, unsigned first, unsigned count,
                                 const RGBQUAD *entries) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  struct w32_dib *s = gdi_target(d);
  if (!s || s->bpp > 8 || !entries || first >= s->color_count)
    return 0;
  if (count > s->color_count - first)
    count = s->color_count - first;
  for (unsigned i = 0; i < count; i++)
    s->colors[first + i] = ((DWORD)entries[i].rgbRed << 16) |
                           ((DWORD)entries[i].rgbGreen << 8) |
                           entries[i].rgbBlue;
  gdi_damage(d, (RECT){0, 0, (LONG)s->width, (LONG)s->height});
  return count;
}
unsigned W32API GetDIBColorTable(HDC h, unsigned first, unsigned count,
                                 RGBQUAD *entries) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  struct w32_dib *s = gdi_target(d);
  if (!s || s->bpp > 8 || !entries || first >= s->color_count)
    return 0;
  if (count > s->color_count - first)
    count = s->color_count - first;
  for (unsigned i = 0; i < count; i++) {
    DWORD c = s->colors[first + i];
    entries[i] =
        (RGBQUAD){(uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16), 0};
  }
  return count;
}
