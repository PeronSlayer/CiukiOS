/* Win32 font statistics of the gdi32 selection layer (f5-10C) and of the
 * PE-to-native font bridge (f5-10C2). Free of GDI/USER handle typedefs so
 * both header families can include it. SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CIUKI_W32_FONT_STATS_H
#define CIUKI_W32_FONT_STATS_H
#include "base.h"
struct w32_gdi_font_stats {
  uint32_t faces, open_faces, face_bytes, face_peak, glyphs, glyph_bytes,
      glyph_peak, total_peak, budget, hits, misses, evictions, face_evictions,
      store_bytes;
};
/* The bridge client of this process and, in service_*, the shared w32srv
 * engine (zero when w32srv does not answer; the call then returns FALSE). */
struct w32_gdi_bridge_stats {
  uint32_t requests, glyphs, cmap_pages, refusals, errors, last_error,
      open_faces;
  uint32_t service_open_faces, service_face_bytes, service_face_peak,
      service_budget, service_glyphs, service_opens, service_evictions,
      service_refusals;
};
void W32API w32_gdi_font_statistics(struct w32_gdi_font_stats *);
int32_t W32API w32_gdi_bridge_statistics(struct w32_gdi_bridge_stats *);
#endif
