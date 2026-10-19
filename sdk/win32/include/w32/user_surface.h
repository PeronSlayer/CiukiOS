/* SPDX-License-Identifier: LGPL-2.1-or-later
 * USER/GDI handoff v1. All rectangles use half-open pixel coordinates. */
#ifndef CIUKI_USER_SURFACE_H
#define CIUKI_USER_SURFACE_H
#include <w32/user32.h>
struct u32_surface {
  void *pixels;
  DWORD width, height, pitch, format;
  int fd;
};
/* USER retains the surface until every DC is released. Origin is client (0,0)
 * in root-surface coordinates. Include/exclude rectangles are in that surface.
 * GDI must intersect its own clipping with this immutable visible clip.
 * release returns root-surface damage rectangles (up to capacity). USER
 * coalesces publication and retains separate rectangles below the region cap.
 * The icon callback consumes ARGB pixels and one-byte AND masks, no HWND.
 * region callbacks operate on GDI's HRGN; no shared representation is assumed.
 * The production USER adapter binds f5-10B w32_gdi_surface_dc, visible
 * regions, DeleteDC and the drawing helpers; no second GDI engine is embedded.
 */
struct u32_gdi {
  DWORD version;
  HDC (*acquire)(const struct u32_surface *, POINT, const RECT *, UINT,
                 const RECT *, UINT);
  UINT (*release)(HDC, RECT *, UINT);
  BOOL (*fill)(HDC, const RECT *, HBRUSH);
  BOOL (*icon)(HDC, int, int, int, int, const DWORD *, const uint8_t *, UINT,
               UINT);
  UINT (*region_rects)(HRGN, RECT *, UINT);
  BOOL (*region_set)(HRGN, const RECT *, UINT);
  /* FALSE snapshots a bitmap; TRUE releases the snapshot (surface non-NULL),
   * or deletes an owned bitmap (surface NULL). */
  BOOL (*bitmap)(HANDLE, struct u32_surface *, BOOL);
  HANDLE (*bitmap_create)(const struct u32_surface *);
};
/* Install before USER use; the table and callbacks outlive the process. Host
 * adapters replace transport only; public USER APIs execute the same code. */
void u32_set_gdi(const struct u32_gdi *);
#endif
