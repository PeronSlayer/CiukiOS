# Subroadmap SR-VIDEO-002 — Graphics & Video Overhaul

> Status: ACTIVE (urgent). Baseline: `CiukiOS Alpha v0.7.1`.
> Supersedes the work previously tracked inline in handoffs about `video.c`
> flicker fixes. Pairs with `docs/roadmap-ciukios-doom.md` milestone
> **M7 — Graphics ready for DOOM/OpenGEM**.

## Goal
Deliver a production-grade video/graphics stack for CiukiOS capable of:
1. Smooth flicker-free text + GUI rendering at arbitrary GOP resolutions
   (baseline 1024×768 up to FHD 1920×1080).
2. A real 2D software rasterizer (pixels, lines, rects, triangles, circles,
   blits, masked blits, alpha blend).
3. An image subsystem (BMP/PCX decoder for splash + app assets).
4. A stable **compositor ABI** exposed to COM programs via the services
   struct, so DOS binaries and OpenGEM can draw through CiukiOS without
   hitting the framebuffer directly.
5. Correct DOS INT 10h semantics for legacy text-mode apps (mode 0x03)
   and VESA VBE-like calls for graphics-mode apps (mode 0x13, 0x12, 0x4F02).

The north-star goal that gates this subroadmap:
**Run OpenGEM with a stable visible desktop, then run DOOM in VGA 320×200×8.**

## Non-Goals
1. Hardware-accelerated GPU drivers (QXL, VirtIO-GPU): deferred.
2. GDI/Win32-style window manager: the desktop stays minimal.
3. Audio.

## Current State Snapshot (2026-04-17)
Completed recently:
- `mem_copy` / `mem_set32` → `rep movsq` / `rep stosq` inline asm.
- Contiguous full-width fast path in `video_present*`.
- Compositor API: `video_begin_frame` / `video_end_frame` with
  `g_frame_scope_depth` to suppress implicit mid-frame presents from
  `video_putchar('\n')` and `video_write`.
- `draw_char` rewritten: per-glyph single `video_mark_dirty`, rows built
  in a local u32 buffer and copied via `mem_copy32`.
- `\b` now erases the glyph cell (fix for CIUKEDIT backspace).
- Splash progress loop wrapped in `begin_frame`/`end_frame`.
- Desktop scene (`shell_run_desktop_session`, `ui_enter_desktop_scene`)
  migrated to compositor API.

Still problematic:
- **P0**: Residual flicker perceived on splash footer & desktop during
  transitions. Root cause hypothesis: pacing / partial backbuffer reuse
  between frames + possibly copying to a fb page while GOP is mid-scanout.
- **P1**: No real 2D drawing API (only `video_fill_rect`, `video_put_pixel`,
  `video_blit_row`). Missing: line, triangle, circle, generic blit, alpha.
- **P1**: No image decoding (splash uses baked PPM→C header). Cannot load
  arbitrary BMP/PCX from FAT.
- **P1**: No COM-visible graphics ABI. COM programs cannot draw through
  CiukiOS. OpenGEM writes directly to fb (hence garbled chars).
- **P2**: INT 10h support is text-only and partial.
- **P2**: VBE info block stub exists but no mode-switch path for 320×200×8
  linear-fb emulation (DOOM requires this).
- **P2**: No palette management for 8bpp (required for DOS games).

## Architecture Overview

### Layers (target)
```
+---------------------------------------------------+
| COM / DOS apps (INT 10h, INT 21h, CIUKI services) |
+--------------------------^------------------------+
                           |
+--------------------------v------------------------+
| Graphics Services ABI (ciuki_gfx_services_t)      |
|   - framebuffer descriptor                        |
|   - draw primitives (line/rect/tri/circle/blit)   |
|   - palette mgmt, mode switch, vsync hint          |
+--------------------------^------------------------+
                           |
+--------------------------v------------------------+
| 2D Rasterizer (stage2/src/gfx2d.c)                |
|   - clipping                                      |
|   - bresenham, scanline fill, circle midpoint     |
|   - alpha over, masked blit                       |
+--------------------------^------------------------+
                           |
+--------------------------v------------------------+
| Video Driver (stage2/src/video.c)                 |
|   - backbuffer + dirty rect                       |
|   - present scheduler (pacing + explicit frames)  |
|   - font + text primitives                        |
+--------------------------^------------------------+
                           |
+--------------------------v------------------------+
| UEFI GOP framebuffer (linear, 32bpp typical)      |
+---------------------------------------------------+
```

### Compositor contract (public)
Every graphical UI layer MUST render with:
```c
video_begin_frame();
  /* draw scene fully */
video_end_frame();
```
- Inside a frame scope, all draw helpers and text writes accumulate into
  the backbuffer only; no present happens.
- `video_end_frame()` commits the dirty bounding box in one contiguous
  `rep movsq` to the GOP framebuffer.
- Pacing (`video_pacing_should_present`) only applies to implicit/legacy
  presents; explicit `video_end_frame` bypasses it.

## Milestones

### M-V2.0 — Flicker elimination (P0, IN PROGRESS)
1. Introduce an **ordered compositor**: dirty-rect is computed per frame;
   the present path uses **non-temporal stores** (`movntdq` / `movnti`)
   to bypass the CPU cache when writing to the GOP fb, preventing
   coherence-related tearing.
2. Make all UI redraws use `video_begin_frame`/`video_end_frame`.
   Grep for remaining `video_present()` / `video_present_dirty()` calls
   and migrate them.
3. Add a `video_clear_backbuffer_rect(x,y,w,h,rgb)` that fills without
   dirtying beyond the rect (prevents unnecessary full-screen commits).
4. Add a `video_fb_barrier()` helper (`mfence`) called before committing
   to the fb to flush any pending WC writes.
5. Serial-logged frame counters (`[video] frame N commit=Xms`) behind
   a `VIDEO_DEBUG_FRAMES` compile flag.

Exit criteria:
- Splash footer shows no flicker for 1000 visual frames (manual observation).
- Desktop scene key-input response < 33 ms and no partial rendering.
- `make test-stage2` green.

### M-V2.1 — 2D rasterizer (P1)
New file: `stage2/src/gfx2d.c` + `stage2/include/gfx2d.h`.
Primitives (all clipped to framebuffer bounds; 32bpp native, 16bpp via
`rgb_to_rgb565`):
1. `gfx2d_pixel(x,y,rgb)`
2. `gfx2d_line(x0,y0,x1,y1,rgb)` — Bresenham.
3. `gfx2d_rect(x,y,w,h,rgb)` — outline.
4. `gfx2d_fill_rect(x,y,w,h,rgb)` — delegates to `video_fill_rect`.
5. `gfx2d_hline(x,y,w,rgb)` / `gfx2d_vline(x,y,h,rgb)`.
6. `gfx2d_circle(cx,cy,r,rgb)` — midpoint algorithm.
7. `gfx2d_fill_circle(cx,cy,r,rgb)` — scanline fill.
8. `gfx2d_tri(x0,y0,x1,y1,x2,y2,rgb)` — outline.
9. `gfx2d_fill_tri(x0,y0,x1,y1,x2,y2,rgb)` — top-flat / bottom-flat split.
10. `gfx2d_blit(src, sw, sh, src_stride, dst_x, dst_y)` — 32bpp src.
11. `gfx2d_blit_masked(src, mask_rgb, …)` — transparent color key.
12. `gfx2d_blit_alpha(src8a, …)` — src has alpha channel, OVER blend.
13. `gfx2d_set_clip(x,y,w,h)` / `gfx2d_clear_clip()`.

All primitives mark dirty rects once per call.

Exit criteria:
- New `gfx` shell command: `gfx line 10 10 100 100` etc. renders live.
- Screenshot-style test: `gfx test-pattern` draws a known pattern; serial
  emits `[gfx] test pattern v1 OK` after no assertion failures.

### M-V2.2 — Image subsystem (P1)
New file: `stage2/src/image.c` (`image_bmp_decode`, `image_pcx_decode`).
- Support BMP 32bpp BI_RGB and 24bpp BI_RGB (top-down + bottom-up).
- Support PCX 8bpp (for DOS game assets).
- Support linear palette conversion (`image_convert_8bpp_to_32bpp`).
- Expose via `shell_cmd_image`: `image show /SYSTEM/WALL.BMP`.

Exit criteria:
- `image show` renders a known test BMP from FAT at cursor pos.
- BMP round-trip hash matches source pixels via a test in
  `scripts/test_image_decode.sh`.

### M-V2.3 — Graphics services ABI (P1)
Extend `boot/proto/services.h`:
```c
typedef struct ciuki_gfx_services {
    void (*begin_frame)(void);
    void (*end_frame)(void);
    void (*put_pixel)(u32 x, u32 y, u32 rgb);
    void (*fill_rect)(u32 x, u32 y, u32 w, u32 h, u32 rgb);
    void (*line)(u32 x0, u32 y0, u32 x1, u32 y1, u32 rgb);
    void (*blit)(const u32 *src, u32 sw, u32 sh, u32 stride, u32 dx, u32 dy);
    void (*get_fb_info)(ciuki_fb_info_t *out);
    u8   (*set_mode)(u8 mode); /* 0x03 text, 0x13 320x200x8, 0x12 640x480x4 */
    void (*set_palette)(u32 first, u32 count, const u8 *rgb_triples);
    u8 reserved[32];
} ciuki_gfx_services_t;
```
- Add to `ciuki_dos_services_t` as `const ciuki_gfx_services_t *gfx;`.
- COM catalog bumps ABI version.

Exit criteria:
- New sample COM `com/gfxsmoke/` draws a red rect, yellow triangle, blue
  circle, and writes `[gfxsmoke] OK` via INT 21h AH=09h.
- `scripts/test_gfxsmoke.sh` runs it in QEMU headless and greps the OK.

### M-V2.4 — INT 10h and VBE emulation (P2)
- Implement INT 10h AH=00h (set video mode) for modes 0x03 (text 80×25)
  and 0x13 (320×200×8 linear).
- On mode 0x13: allocate a logical 320×200 8bpp plane, map it to
  `0xA0000`-equivalent window (virtual; actual physical is the backbuffer
  area), upscale/letterbox into the real GOP fb at present time.
- INT 10h AH=0Ch (write pixel), AH=0Dh (read pixel).
- INT 10h AH=0Fh (get current mode).
- VBE: INT 10h AX=4F00h / 4F01h / 4F02h (mode set) minimal.

Exit criteria:
- DOS sample that writes to `0xA0000` shows a gradient in QEMU.
- OpenGEM launches and renders correctly (no garbled chars).
- DOOM boot sequence reaches the title screen (may still need audio off).

### M-V2.5 — Palette and timing (P2)
- 256-color palette table (`g_palette_8bpp[256]`).
- `palette_set(first,count,rgb_triples)` binding.
- Vsync hint via `video_pacing_wait_scanline_out()` (polling a tick-based
  approximation; real GOP has no vblank).
- Double-buffered present rate capped at 60 fps.

Exit criteria:
- DOOM palette fades work (intermission screens don't flicker).
- DOOM in-game FPS measured via serial tagged `[doom] fps=N`.

### M-V2.6 — Desktop WM polish (P2, optional)
- Window move via keyboard, focus ring, basic menu.
- Not required for DOOM/OpenGEM gate.

## Validation Gates

| Stage      | Test                                      | Expected                          |
|------------|-------------------------------------------|-----------------------------------|
| V2.0       | Headless QEMU 30s idle boot               | No "[video] present_coalesced" spikes |
| V2.0       | Manual splash watch                       | No flicker, smooth progress bar   |
| V2.1       | `gfx test-pattern`                        | All primitives visible, serial OK |
| V2.2       | `image show /SYSTEM/WALL.BMP`             | Image matches reference sha256    |
| V2.3       | `run GFXSMOKE.COM`                        | serial "[gfxsmoke] OK"            |
| V2.4       | `run DOSMODE13.COM`                       | 320×200 gradient visible          |
| V2.4       | `opengem`                                 | Desktop UI readable, not garbled  |
| V2.5       | `doom -warp 1 1 -timedemo demo1`          | Title + demo plays, fps logged    |

## File Layout
```
stage2/
  include/
    video.h          (existing + frame scope + clipping)
    gfx2d.h          (NEW, M-V2.1)
    image.h          (NEW, M-V2.2)
  src/
    video.c          (existing; non-temporal stores in M-V2.0)
    gfx2d.c          (NEW, M-V2.1)
    image.c          (NEW, M-V2.2)
boot/proto/
  services.h         (extended in M-V2.3)
com/
  gfxsmoke/          (NEW, M-V2.3)
  dosmode13/         (NEW, M-V2.4)
docs/
  subroadmap-sr-video-002.md  (this file)
  handoffs/YYYY-MM-DD-video-vM-V2.*.md
scripts/
  test_gfx_pattern.sh         (NEW)
  test_image_decode.sh        (NEW)
  test_gfxsmoke.sh            (NEW)
```

## Risks
1. **Non-temporal stores may regress on non-Intel CPUs** — guard with
   CPUID check or a `VIDEO_NONTEMPORAL=0` compile flag.
2. **Mode 0x13 emulation cost** — upscaling 320×200 → 1920×1080 per
   frame is a 6x nearest-neighbor blit; budget ~8 ms on an Intel CPU
   with ERMSB, acceptable for DOOM's 35 fps target.
3. **Palette animation** — DOOM relies on fast palette swap; each swap
   currently requires a full re-upscale; cache the scaled frame only
   when palette is dirty.
4. **Backward compatibility of services ABI** — bump ABI version on any
   struct change; COM binaries probe size field.

## Versioning Plan
- Complete M-V2.0 → bump to `v0.7.2` (flicker-free baseline).
- Complete M-V2.1 + M-V2.2 → bump to `v0.8.0` (2D + images).
- Complete M-V2.3 + M-V2.4 → bump to `v0.9.0` (DOS-era graphics ready).
- Complete M-V2.5 → bump to `v0.9.1` (DOOM-ready).
- DOOM title screen visible on CiukiOS in QEMU → `v1.0.0-rc1`.

## Next Action (Sprint 1)
1. Migrate remaining `video_present()` / `video_present_dirty()` sites
   to frame-scope blocks. List from grep:
   - `stage2/src/stage2.c:325` (`draw_title_bar`)
   - `stage2/src/shell.c` (multiple)
   - `stage2/src/ui.c:407` (desktop render loop)
   - `stage2/src/video.c:414` (mode init)
2. Implement `video_fb_barrier()` + optional non-temporal copy.
3. Close M-V2.0 validation, bump to v0.7.2, commit, handoff.

## Last Updated
2026-04-17
