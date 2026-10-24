/* SPDX-License-Identifier: LGPL-2.1-or-later
 * GetFreeSystemResources (G30, SY-3). Windows 98 reports the percentage of
 * the free 16-bit USER and GDI heaps; this implementation has no such heaps,
 * so USER is the free share of the window table, GDI is the free share of
 * gdi32's object table (w32_gdi_live_objects, resolved on first use), and
 * the system value is the lower of the two, as Windows 98 defines it. The
 * values are those of the calling process; the desktop's Resource Meter
 * (apps/systools) reports the desktop-wide tables. */
#include "internal.h"
extern HANDLE W32API GetModuleHandleA(const char *);
extern void *W32API GetProcAddress(HANDLE, const char *);
#define GFSR_SYSTEMRESOURCES 0
#define GFSR_GDIRESOURCES 1
#define GFSR_USERRESOURCES 2
#define GDI_CAPACITY 4074u /* gdi32 handle slots, less the 21 stock objects */
static UINT free_percent(UINT used, UINT capacity) {
  if (!capacity || used >= capacity)
    return 0;
  return (UINT)((uint64_t)(capacity - used) * 100 / capacity);
}
UINT W32API GetFreeSystemResources(UINT selector) {
  U32_GUARD;
  if (selector > GFSR_USERRESOURCES)
  {
    u32_fail(87);
    return 0;
  }
  UINT used = 0;
  for (UINT i = 0; i < U32_WINDOWS; i++)
    if (u32_windows[i].handle)
      used++;
  UINT user = free_percent(used, U32_WINDOWS), gdi = 100;
  HANDLE module = GetModuleHandleA("gdi32.dll");
  DWORD(W32API * live)(void) =
      module ? (DWORD(W32API *)(void))GetProcAddress(module, "w32_gdi_live_objects") : 0;
  if (live)
    gdi = free_percent(live(), GDI_CAPACITY);
  return selector == GFSR_USERRESOURCES ? user
         : selector == GFSR_GDIRESOURCES ? gdi
                                         : (user < gdi ? user : gdi);
}
