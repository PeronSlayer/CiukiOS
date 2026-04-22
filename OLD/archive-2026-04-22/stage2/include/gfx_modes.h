#ifndef STAGE2_GFX_MODES_H
#define STAGE2_GFX_MODES_H

#include "types.h"

/*
 * CiukiOS Graphics Modes — M-V2.4 / M-V2.5.
 *
 * Provides compatibility shims for classic DOS graphics modes on top of
 * the UEFI GOP linear 32bpp framebuffer. The active plane is a
 * virtual surface that gets upscaled (nearest-neighbor, letterboxed)
 * into the real backbuffer when presented.
 *
 * Supported modes:
 *   0x03  text 80x25 (the default stage2 console; no plane, passthrough)
 *   0x13  320x200 8bpp planar (DOOM / DOS games)
 *
 * Stubs for VBE extended modes (4F00h / 4F01h / 4F02h) route through
 * gfx_mode_vbe_* helpers.
 */

/* Mode constants (BIOS INT 10h AH=00h compatible). */
#define GFX_MODE_TEXT_80x25   0x03U
#define GFX_MODE_VGA_320x200  0x13U

/* Plane descriptor (mode 0x13). */
#define GFX_MODE13_W  320U
#define GFX_MODE13_H  200U

/* Core API */
void gfx_mode_init(void);
u8   gfx_mode_current(void);
u8   gfx_mode_set(u8 mode);

/* Mode 0x13 plane manipulation */
u8  *gfx_mode13_plane(void);
void gfx_mode13_put_pixel(u32 x, u32 y, u8 color_index);
u8   gfx_mode13_get_pixel(u32 x, u32 y);
void gfx_mode13_clear(u8 color_index);

/* Palette (256 entries; triples packed as 6-bit r,g,b like real VGA) */
void gfx_palette_set(u32 first, u32 count, const u8 *rgb_triples_6bit);
void gfx_palette_set_default_vga(void);
u32  gfx_palette_get_rgb(u8 index); /* returns 0x00RRGGBB */

/* Palette fade — blend the whole palette `step`/`total` toward a target 24-bit
 * RGB color. step=0 leaves the current palette; step=total snaps to target.
 * Useful for DOOM-style screen fades (blood flash, intermission, title wipes).
 * Marks palette dirty for the next `gfx_mode_present`.
 */
void gfx_palette_fade(u32 target_rgb, u32 step, u32 total);

/* Mode 0x13 bulk fills (fast path DOOM-style). */
void gfx_mode13_fill(u8 color_index);
void gfx_mode13_fill_rect(u32 x, u32 y, u32 w, u32 h, u8 color_index);

/* Blit an 8-bit indexed bitmap (stride in bytes) onto the mode 0x13 plane.
 * Clips to plane bounds. Transparent color is `transparent_idx` when
 * `use_transparent` != 0 (matches DOOM's 8-bit masked patches). */
void gfx_mode13_blit_indexed(const u8 *src, u32 sw, u32 sh, u32 stride,
                             u32 dx, u32 dy,
                             u8 use_transparent, u8 transparent_idx);

/* Signed/clipped indexed blit for real patch/sprite placement: destination
 * may be partially off-screen and source cropping is derived automatically. */
void gfx_mode13_blit_indexed_clip(const u8 *src, u32 sw, u32 sh, u32 stride,
                                  i32 dx, i32 dy,
                                  u8 use_transparent, u8 transparent_idx);

/* Single-column draw (R_DrawColumn style). Writes `h` indices starting at
 * (x, y), taking one byte per row from `src` (stride = 1 default). Clips. */
void gfx_mode13_draw_column(u32 x, u32 y, u32 h, const u8 *src);

/* Read back N palette entries as 6-bit VGA triples (out[3*i + {0..2}]). */
void gfx_palette_get_raw(u32 first, u32 count, u8 *rgb_triples_6bit_out);

/* Nearest-neighbor scaled blit of an 8-bit indexed source onto the mode 0x13
 * plane. Produces a dw*dh rectangle at (dx,dy) from a sw*sh source with
 * optional chroma-key. Used for HUD patch / title-screen scaling. Clips. */
void gfx_mode13_blit_scaled(const u8 *src, u32 sw, u32 sh, u32 stride,
                            u32 dx, u32 dy, u32 dw, u32 dh,
                            u8 use_transparent, u8 transparent_idx);

/* Signed/clipped variant of the scaled blit. Allows partially off-screen HUD,
 * title, or sprite patches while keeping nearest-neighbor sampling stable. */
void gfx_mode13_blit_scaled_clip(const u8 *src, u32 sw, u32 sh, u32 stride,
                                 i32 dx, i32 dy, u32 dw, u32 dh,
                                 u8 use_transparent, u8 transparent_idx);

/* Masked single-column draw: same as `gfx_mode13_draw_column` but skips
 * pixels equal to `transparent_idx` (DOOM R_DrawMaskedColumn path). */
void gfx_mode13_draw_column_masked(u32 x, u32 y, u32 h, const u8 *src,
                                   u8 transparent_idx);

/* Sampled masked column draw: destination height is independent from source
 * height and texels are fetched from src[(frac >> 16)] with frac advancing by
 * frac_step_16_16 per output pixel. Clips signed destination Y. */
void gfx_mode13_draw_column_sampled_masked(i32 x, i32 y, u32 h,
                                           const u8 *src, u32 src_h,
                                           u32 frac_16_16,
                                           u32 frac_step_16_16,
                                           u8 transparent_idx);

/* Draw a classic DOOM patch-format lump (column directory + post lists).
 * `x,y` are the logical patch anchor coordinates; the patch's internal
 * left/top offsets are applied automatically. Silently ignores malformed
 * patch data beyond `patch_size`. */
void gfx_mode13_draw_doom_patch(const u8 *patch, u32 patch_size,
                                i32 x, i32 y);

/* Return the frame counter (monotonically incremented on each successful
 * `gfx_mode_present`). Useful for pacing + FPS measurement. */
u32  gfx_frame_counter(void);

/* Present the active non-text plane into the backbuffer + commit frame.
 * For text mode this is a no-op (console drives its own redraw).
 * Returns 1 if a plane was presented, 0 otherwise.
 */
int  gfx_mode_present(void);

/*
 * OPENGEM-008 — Real-frame first-blit hook.
 *
 * When armed, the next successful mode-13 present emits the frozen
 * serial marker `OpenGEM: desktop frame blitted` exactly once and
 * auto-disarms. Idempotent: arm/disarm are safe to re-enter.
 *
 * The arm is a **correlation hook**, not a blocking primitive: it
 * does not change present-path timing, palette, or dirty-tracking.
 * Purpose: give a runtime gate a boot-log signal that a real mode-13
 * frame reached the backbuffer during an OpenGEM session (as opposed
 * to the semantic "desktop first frame presented" marker from
 * OPENGEM-007 which fires before `shell_run()` dispatch).
 */
void gfx_mode_opengem_arm_first_frame(void);
void gfx_mode_opengem_disarm_first_frame(void);
int  gfx_mode_opengem_first_frame_armed(void);

/* INT 10h dispatcher (BIOS-level). Declared in services.h form. */
struct ciuki_int21_regs;
struct ciuki_dos_context;
void gfx_int10_dispatch(struct ciuki_dos_context *ctx, struct ciuki_int21_regs *regs);

#endif
