# Handoff - Stage2 Graphic Splash + Framebuffer Handoff (2026-04-15)

## Goal completed

Implemented the three requested OT steps:

1. **Framebuffer info in handoff ABI** from UEFI loader to Stage2.
2. **Graphic splash renderer** centered/scaled on framebuffer (with fallback to ASCII renderer).
3. **Shell command to test splash on demand** (`gsplash`, alias `splash`).

## Technical changes

### 1) Handoff ABI (loader -> stage2)
- Extended `handoff_v0_t` with framebuffer metadata:
  - `framebuffer_base`
  - `framebuffer_width`
  - `framebuffer_height`
  - `framebuffer_pitch`
  - `framebuffer_bpp`
- Loader now populates these fields when Stage2 is active.
- Loader now detects GOP pixel depth more robustly (`32bpp`, `16bpp` for bitmask modes).

### 2) Video backend enhancements
- `video` module now exposes framebuffer primitives:
  - `video_ready`, `video_width_px`, `video_height_px`, `video_pitch_bytes`, `video_bpp`
  - `video_fill`, `video_fill_rect`, `video_put_pixel`
- Internal pixel backend supports both:
  - **32-bit RGB** (default GOP path)
  - **16-bit RGB565** conversion path

### 3) Splash renderer
- Added `stage2_splash_show_graphic()` in `splash.c`:
  - converts source ASCII density to grayscale luminance
  - renders directly to framebuffer pixels
  - keeps aspect ratio, scales to fit, centers image
- Boot path in `stage2.c` now:
  - tries **graphic splash** first
  - falls back to previous text/ascii splash if needed
  - logs mode (`gfx`/`ascii`) and bpp in serial

### 4) Shell preview command
- Added shell command:
  - `gsplash` (alias `splash`)
- Behavior:
  - shows graphic splash preview
  - waits ~1.5s or keypress
  - restores standard shell layout/title bar

## Files touched

- `boot/proto/handoff.h`
- `boot/uefi-loader/loader.c`
- `stage2/include/video.h`
- `stage2/src/video.c`
- `stage2/include/splash.h`
- `stage2/src/splash.c`
- `stage2/src/stage2.c`
- `stage2/src/shell.c`

## Validation

All tests passed after integration:

- `make test-stage2`
- `make test-fallback`
- `make test-fat-compat`

