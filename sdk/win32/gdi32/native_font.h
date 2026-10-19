/* Native side of the f5-10C font bridge. SPDX-License-Identifier:
 * LGPL-2.1-or-later */
#ifndef W32_GDI_NATIVE_FONT_H
#define W32_GDI_NATIVE_FONT_H
#include <w32/gdi32.h>
/* Opens an owned SDK face; DeleteObject/process detach closes it. This adapter
 * is native code; the lead's PE-to-native bridge installs these callbacks. */
HFONT w32_gdi_native_font_open(const char *, unsigned);
#endif
