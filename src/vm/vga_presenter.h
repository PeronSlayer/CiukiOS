/* CiukiOS bounded presenter for the software VGA model.
 *
 * Converts scanout (cvga_render_row8 indexes + DAC) into a native 15/16/24/
 * 32-bit surface: a protected framebuffer mapping or a compositor band. It
 * never calls firmware, changes CPU mode or touches VGA ports.
 *
 * Damage: rows are redrawn when their plane bytes changed (model dirty bits),
 * when scanout registers/DAC changed (display_changes), when the window,
 * visible clip list, format or geometry changed, or on request. Work per call is bounded by
 * budget_pixels; an incomplete frame resumes on the next call. The presenter
 * is the only consumer of cvga_state.dirty and clears the bits it takes.
 */
#ifndef CIUKIOS_VGA_PRESENTER_H
#define CIUKIOS_VGA_PRESENTER_H

#include <stddef.h>
#include "virtual_vga.h"

#define CVP_MAX_WIDTH 2048u
#define CVP_MAX_HEIGHT 2048u
#define CVP_MAX_CLIPS 16u
#define CVP_MAX_ROWS 1024u              /* geometry rows tracked for damage */

#define CVP_FORCE_FULL 1u               /* redraw every row */
#define CVP_BLINK_VISIBLE 2u            /* text blink phase */
#define CVP_CURSOR_VISIBLE 4u           /* text cursor phase */
#define CVP_NO_DAMAGE 8u                /* render clips now; keep damage state */

enum { CVP_OK = 0, CVP_BAD_FORMAT = 1, CVP_BAD_RECT = 2, CVP_UNSUPPORTED = 3,
       CVP_BAD_STORAGE = 4 };

typedef struct cvp_rect { int16_t left, top, right, bottom; } cvp_rect; /* exclusive */

typedef struct cvp_format {
    uint32_t pitch;                     /* bytes between target rows */
    uint16_t width, height;             /* target surface size in pixels */
    uint8_t bytes;                      /* 2, 3 or 4 */
    uint8_t red_size, red_pos, green_size, green_pos, blue_size, blue_pos, reserved;
} cvp_format;

typedef struct cvp_request {
    cvp_rect window;                    /* whole guest image scales into this */
    uint16_t clip_count, reserved;      /* 0: the whole window is visible */
    cvp_rect clip[CVP_MAX_CLIPS];
    uint32_t flags, budget_pixels;      /* budget 0 = unbounded */
} cvp_request;

typedef struct cvp_stats {
    uint32_t status, complete;
    uint32_t rows_drawn, pixels_written;
    uint32_t passes;                    /* sweeps that wrapped the window while drawing */
    uint16_t source_width, source_height; /* physical scanlines */
    uint32_t frames_completed, full_redraws;
} cvp_stats;

typedef struct cvp_state {
    uint32_t initialised, display_changes, frames_completed, full_redraws, passes, drawn_since_wrap;
    uint32_t lut_changes, flags_seen;
    cvp_rect window;
    cvp_rect clip[CVP_MAX_CLIPS];      /* visibility of the last frame */
    uint32_t clip_count;
    cvp_format format;
    cvga_geometry geometry;
    uint16_t cursor, width, height, src_width, src_height, repeat;
    uint8_t pending[CVP_MAX_ROWS / 8];
    uint8_t taken[4][CVGA_DIRTY_BYTES];
    uint32_t lut[256];
    uint16_t x_map[CVP_MAX_WIDTH];
    uint16_t y_map[CVP_MAX_HEIGHT];
    uint8_t line8[CVP_MAX_WIDTH];
    uint8_t native[CVP_MAX_WIDTH * 4];
} cvp_state;

/* A single immutable VGA image rendered across multiple compositor bands.
 * The caller zero-initialises and owns this state plus the storage supplied
 * to capture. The storage holds a cvga_state snapshot followed by one native row per source
 * row. Capture invalidates only rows affected by the supplied dirty snapshot,
 * palette/display/geometry/format changes, or text blink/cursor phase. */
typedef struct cvp_retained_state {
    uint32_t magic, initialised, storage_bytes;
    uint32_t display_changes, captured_images, source_rows_converted;
    uint32_t window_width, window_height, source_width, source_height;
    uint32_t phases, row_bytes;
    const cvga_state *snapshot;
    uint8_t *converted;
    cvp_format format;
    cvga_geometry geometry;
    uint16_t x_map[CVP_MAX_WIDTH];
    uint16_t y_map[CVP_MAX_HEIGHT];
    uint8_t valid[CVP_MAX_ROWS];
    uint8_t line8[CVP_MAX_WIDTH];
    uint32_t lut[256];
} cvp_retained_state;

void CVGA_CALL cvp_init(cvp_state *s);
/* target addresses pixel (0,0) of a surface at least format->pitch *
 * format->height bytes long; the caller validates that extent. */
int CVGA_CALL cvp_present(cvp_state *s, cvga_state *v, const cvp_format *format,
                          const cvp_request *request, uint8_t *target, cvp_stats *stats);

/* Required caller-owned storage: one VGA snapshot plus cached native source
 * rows. Returns zero when geometry or the requested output row size is not
 * supported. Storage must be 4-byte aligned and at least this large. */
size_t CVGA_CALL cvp_retained_bytes(const cvga_state *live, unsigned window_width,
                                    unsigned bytes);

/* logical_format describes the full stable guest output surface (not one
 * scratch band), with width/height equal to window_width/window_height. The
 * dirty maps are an optional snapshot taken by the caller before clearing
 * live->dirty; pass NULL only when no source rows changed or force is set.
 * force invalidates every cached source row. */
int CVGA_CALL cvp_retained_capture(cvp_retained_state *s, const cvga_state *live,
                                   const cvp_format *logical_format,
                                   unsigned window_width, unsigned window_height,
                                   unsigned phases, void *storage, size_t capacity,
                                   const uint8_t taken_dirty[4][CVGA_DIRTY_BYTES],
                                   int force);

/* q.window and q.clip are in absolute compositor coordinates. band_left/top
 * give the absolute origin of band_memory; band_format describes that scratch
 * surface and must use the captured pixel format. */
int CVGA_CALL cvp_retained_blit(cvp_retained_state *s,
                                const cvp_format *band_format,
                                const cvp_request *q,
                                int32_t band_left, int32_t band_top,
                                uint8_t *band_memory, cvp_stats *stats);

#endif
