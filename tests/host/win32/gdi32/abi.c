/* PE32 declarations consumed by f5-10A/C. SPDX-License-Identifier:
 * LGPL-2.1-or-later */
#include <w32/gdi32.h>
_Static_assert(sizeof(HDC) == 4 && sizeof(POINT) == 8 && sizeof(RECT) == 16,
               "Win32 handles/geometry");
_Static_assert(sizeof(BITMAP) == 24 && sizeof(BITMAPINFOHEADER) == 40 &&
                   sizeof(DIBSECTION) == 84,
               "DIB ABI");
_Static_assert(sizeof(LOGBRUSH) == 12 && sizeof(LOGPEN) == 16 &&
                   sizeof(RGNDATAHEADER) == 32,
               "GDI object ABI");
_Static_assert(offsetof(struct w32_dib, width) == 4 &&
                   offsetof(struct w32_dib, masks) == 28 &&
                   offsetof(struct w32_dib, colors) == 40 &&
                   offsetof(struct w32_dib, color_count) == 1064 &&
                   sizeof(struct w32_dib) == 1068,
               "USER32 surface bridge ABI");
_Static_assert(sizeof(struct w32_gdi_glyph) == 28 &&
                   sizeof(struct w32_gdi_font_metrics) == 16 &&
                   sizeof(struct w32_gdi_font_provider) == 28,
               "font bridge ABI");
