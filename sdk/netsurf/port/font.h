/* SPDX-License-Identifier: MIT */
#include <libnsfb.h>
#define FB_FONT_HEIGHT 16
nserror ciuki_font_plot(nsfb_t *, const struct plot_font_style *, int, int, const char *, size_t);
void ciuki_font_prefetch(void); /* fx-15: open the body faces on a worker at start-up */
struct ciuki_font_cache_stats;
void ciuki_font_stats(struct ciuki_font_cache_stats *); /* fx-nsperf: glyph cache totals of the open faces */
