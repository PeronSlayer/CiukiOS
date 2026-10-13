/* SPDX-License-Identifier: MIT
 * Scalable, grey-AA TrueType fonts. Link with -lfont. Trusted font files only;
 * see docs/design/fonts-contract.md for the backend's trust requirements. */
#ifndef CIUKI_FONT_H
#define CIUKI_FONT_H
#include <stddef.h>
#include <stdint.h>
#define CIUKI_FONT_CACHE_GLYPHS 128u
#define CIUKI_FONT_CACHE_BYTES (256u * 1024u)
#define CIUKI_FONT_NAME_MAX 128
struct ciuki_font_face;
struct ciuki_font_metrics {
    int ascent, descent, line_gap;                 /* hhea, signed design units */
    unsigned units_per_em, glyph_count;
    int has_os2, typo_ascent, typo_descent, typo_line_gap;
    unsigned win_ascent, win_descent, weight, selection;
    char family[CIUKI_FONT_NAME_MAX], style[CIUKI_FONT_NAME_MAX];
};
struct ciuki_font_bitmap {
    unsigned width, height, stride;
    int left, top;                                /* relative to pen/baseline */
    int32_t advance;                              /* 26.6 pixels */
    uint8_t *coverage;
};
struct ciuki_font_cache_stats {
    unsigned glyphs;
    size_t bytes;
    uint64_t hits, misses, evictions;
};
/* Errors: -1 with errno. Face calls serialize with a per-face mutex.
 * close requires all callers to have stopped using the face. */
int ciuki_font_open(const char *path, struct ciuki_font_face **out);
void ciuki_font_close(struct ciuki_font_face *);
int ciuki_font_get_metrics(struct ciuki_font_face *, struct ciuki_font_metrics *);
int ciuki_font_lookup(struct ciuki_font_face *, uint32_t codepoint, unsigned *glyph);
int ciuki_font_hmetrics(struct ciuki_font_face *, unsigned glyph, int *advance, int *bearing);
/* em_size is 26.6 em pixels, 64..8192. The returned bitmap is an owned copy. */
int ciuki_font_raster(struct ciuki_font_face *, unsigned glyph, unsigned em_size, struct ciuki_font_bitmap *);
void ciuki_font_bitmap_free(struct ciuki_font_bitmap *);
/* UTF-8: explicit byte length, LTR single line, invalid bytes -> U+FFFD.
 * measure returns the same 26.6 advance as paint, including legacy kern. */
int ciuki_font_measure(struct ciuki_font_face *, unsigned em_size, const char *, size_t length, int32_t *advance);
/* Callback borrows cached coverage under the face lock; do not re-enter the
 * face or retain coverage. x/y are integer glyph top-left positions.
 * A nonzero callback return aborts painting and is propagated. */
typedef int (*ciuki_font_painter)(void *, int x, int y, const struct ciuki_font_bitmap *);
int ciuki_font_paint(struct ciuki_font_face *, unsigned em_size, const char *, size_t length,
                     int x, int baseline, ciuki_font_painter, void *, int32_t *advance);
int ciuki_font_cache_info(struct ciuki_font_face *, struct ciuki_font_cache_stats *);
void ciuki_font_cache_clear(struct ciuki_font_face *);
#endif
