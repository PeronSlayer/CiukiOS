/* Shared value-only USER/DirectDraw tuple. SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CIUKI_W32_DISPLAY_LEASE_H
#define CIUKI_W32_DISPLAY_LEASE_H
#include <stdint.h>
struct dd_lease {
  uint32_t active, token, generation, width, height, bpp, pitch;
};
#endif
