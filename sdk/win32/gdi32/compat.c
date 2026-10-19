/* Small public GDI aliases and mapping helpers. SPDX-License-Identifier:
 * LGPL-2.1-or-later */
#include "internal.h"
HDC W32API CreateICA(const char *driver, const char *device, const char *out,
                     const void *mode) {
  return CreateDCA(driver, device, out, mode);
}
HDC W32API CreateICW(const WCHAR *driver, const WCHAR *device, const WCHAR *out,
                     const void *mode) {
  return CreateDCW(driver, device, out, mode);
}
BOOL W32API OffsetViewportOrgEx(HDC h, int x, int y, POINT *old) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d || (int64_t)d->s.viewport_org.x + x < -GDI_LIMIT ||
      (int64_t)d->s.viewport_org.x + x > GDI_LIMIT ||
      (int64_t)d->s.viewport_org.y + y < -GDI_LIMIT ||
      (int64_t)d->s.viewport_org.y + y > GDI_LIMIT)
    return 0;
  return SetViewportOrgEx(h, d->s.viewport_org.x + x, d->s.viewport_org.y + y,
                          old);
}
BOOL W32API OffsetWindowOrgEx(HDC h, int x, int y, POINT *old) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d || (int64_t)d->s.window_org.x + x < -GDI_LIMIT ||
      (int64_t)d->s.window_org.x + x > GDI_LIMIT ||
      (int64_t)d->s.window_org.y + y < -GDI_LIMIT ||
      (int64_t)d->s.window_org.y + y > GDI_LIMIT)
    return 0;
  return SetWindowOrgEx(h, d->s.window_org.x + x, d->s.window_org.y + y, old);
}
static int scale(HDC h, int xn, int xd, int yn, int yd, SIZE *old, int window) {
  struct dc *d = gdi_dc(h);
  if (!d || !xd || !yd)
    return 0;
  SIZE s = window ? d->s.window_ext : d->s.viewport_ext;
  if (d->s.map < MM_ISOTROPIC) {
    if (old)
      *old = s;
    return 1;
  }
  int64_t x = (int64_t)s.cx * xn / xd, y = (int64_t)s.cy * yn / yd;
  if (!x || !y || x < -GDI_LIMIT || x > GDI_LIMIT || y < -GDI_LIMIT ||
      y > GDI_LIMIT)
    return 0;
  return window ? SetWindowExtEx(h, (int)x, (int)y, old)
                : SetViewportExtEx(h, (int)x, (int)y, old);
}
BOOL W32API ScaleViewportExtEx(HDC h, int xn, int xd, int yn, int yd,
                               SIZE *old) {
  GDI_GUARD;
  return scale(h, xn, xd, yn, yd, old, 0);
}
BOOL W32API ScaleWindowExtEx(HDC h, int xn, int xd, int yn, int yd, SIZE *old) {
  GDI_GUARD;
  return scale(h, xn, xd, yn, yd, old, 1);
}
BOOL W32API ResizePalette(HPALETTE h, unsigned count) {
  GDI_GUARD;
  struct object *o = gdi_object(h, GDI_PALETTE);
  if (!o || o->stock || !count || count > 256)
    return 0;
  for (unsigned i = o->u.palette.count; i < count; i++)
    o->u.palette.entries[i] = (PALETTEENTRY){0};
  o->u.palette.count = count;
  o->u.palette.dirty = 1;
  return 1;
}
unsigned W32API GetSystemPaletteUse(HDC h) {
  GDI_GUARD;
  return gdi_dc(h) ? 1 : 0;
}
unsigned W32API SetSystemPaletteUse(HDC h, unsigned use) {
  GDI_GUARD;
  return gdi_dc(h) && use == 1 ? 1 : 0;
}
DWORD W32API GdiSetBatchLimit(DWORD limit) {
  (void)limit;
  return 1;
}
BOOL W32API GetTextExtentPointA(HDC h, const char *s, int n, SIZE *out) {
  return GetTextExtentPoint32A(h, s, n, out);
}
BOOL W32API GetTextExtentPointW(HDC h, const WCHAR *s, int n, SIZE *out) {
  return GetTextExtentPoint32W(h, s, n, out);
}
