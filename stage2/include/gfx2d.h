#ifndef STAGE2_GFX2D_H
#define STAGE2_GFX2D_H

#include "types.h"

/*
 * CiukiOS 2D software rasterizer (M-V2.1).
 *
 * All draw primitives:
 *   - Operate on the current backbuffer via the video driver.
 *   - Clip to framebuffer bounds (or to the active clip rect when set).
 *   - Mark a dirty rectangle exactly once per call.
 *   - Do NOT trigger a present; the caller owns frame boundaries
 *     (wrap in video_begin_frame / video_end_frame for atomic commits).
 *   - Accept 0x00RRGGBB color format.
 */

/* Clip rect management */
void gfx2d_set_clip(u32 x, u32 y, u32 w, u32 h);
void gfx2d_clear_clip(void);

/* Points / lines */
void gfx2d_pixel(u32 x, u32 y, u32 rgb);
void gfx2d_hline(u32 x, u32 y, u32 w, u32 rgb);
void gfx2d_vline(u32 x, u32 y, u32 h, u32 rgb);
void gfx2d_line(i32 x0, i32 y0, i32 x1, i32 y1, u32 rgb);

/* Rectangles */
void gfx2d_rect(u32 x, u32 y, u32 w, u32 h, u32 rgb);        /* outline */
void gfx2d_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 rgb);   /* filled  */

/* Circles */
void gfx2d_circle(i32 cx, i32 cy, u32 r, u32 rgb);           /* outline */
void gfx2d_fill_circle(i32 cx, i32 cy, u32 r, u32 rgb);      /* filled  */

/* Triangles */
void gfx2d_tri(i32 x0, i32 y0, i32 x1, i32 y1, i32 x2, i32 y2, u32 rgb);
void gfx2d_fill_tri(i32 x0, i32 y0, i32 x1, i32 y1, i32 x2, i32 y2, u32 rgb);

/* Blits (source is 32bpp 0xXXRRGGBB laid out tightly with `stride` u32 per row) */
void gfx2d_blit(const u32 *src, u32 sw, u32 sh, u32 stride, u32 dx, u32 dy);
/* Masked blit: pixels equal to `key_rgb` are NOT written. */
void gfx2d_blit_masked(const u32 *src, u32 sw, u32 sh, u32 stride,
                       u32 dx, u32 dy, u32 key_rgb);
/* Alpha-over blit: top byte of each src pixel is alpha 0..255. */
void gfx2d_blit_alpha(const u32 *src_argb, u32 sw, u32 sh, u32 stride,
                      u32 dx, u32 dy);

/* Test pattern used by `gfx test-pattern` for visual regression. */
void gfx2d_test_pattern(void);

#endif /* STAGE2_GFX2D_H */
