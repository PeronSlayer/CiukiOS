/* Shared adapter for USER and dialogs tests of production GDI pixel formats.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/pixelformat.h>

int host_pixel_format(DWORD dc, int set) {
  return set ? SetPixelFormat(dc, set, 0) : GetPixelFormat(dc);
}
