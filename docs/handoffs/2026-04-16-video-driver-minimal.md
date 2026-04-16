# Handoff: Minimal Video Driver (Double Buffering + Scanline Blitting + GOP Mode Selection)

**Date:** 2026-04-16
**Branch:** `feature/copilot-video-driver-minimal`
**Commit:** 50d1cd4

## Context and Goal

All CiukiOS rendering previously wrote directly to the GOP framebuffer (MMIO write-combined memory). This caused:
- Visible flicker/tearing in desktop and splash
- Extremely slow `scroll_up()` due to byte-by-byte read-back from MMIO
- ~786K individual `video_put_pixel()` calls in splash rendering
- No GOP mode selection (firmware default only)

Goal: implement a minimal video driver with double buffering, scanline-optimized blitting, and basic GOP mode selection to eliminate flicker and improve rendering performance.

## Files Touched

| File | Action | Description |
|------|--------|-------------|
| `stage2/include/mem.h` | NEW | Shared memory primitive declarations |
| `stage2/src/mem.c` | NEW | `mem_copy`, `mem_set`, `mem_copy32`, `mem_set32` with 8-byte aligned fast paths |
| `stage2/src/video.c` | MODIFIED | Back buffer infrastructure, `video_present()`, `video_blit_row()`, render target indirection |
| `stage2/include/video.h` | MODIFIED | Added `video_present()` and `video_blit_row()` declarations |
| `stage2/src/stage2.c` | MODIFIED | Added `video_present()` at 4 boot sequence points |
| `stage2/src/ui.c` | MODIFIED | Added `video_present()` after desktop scene enter |
| `stage2/src/shell.c` | MODIFIED | Added `video_present()` at 5 points (prompt, command output, desktop render loop, desktop exit) |
| `stage2/src/splash.c` | MODIFIED | Replaced per-pixel rendering with scanline buffer + `video_blit_row()` |
| `boot/uefi-loader/loader.c` | MODIFIED | Added GOP mode enumeration and preferred mode selection |

## Architecture Decisions

### 1. Static BSS Back Buffer (800x600x4 = ~1.9MB)
- Buffer: `static u8 g_backbuf[VIDEO_BACKBUF_MAX_BYTES]` in BSS, 64-byte aligned
- Sized for 800x600 32bpp to keep stage2 BSS under UEFI firmware allocation limits
- If framebuffer resolution exceeds buffer, falls back to direct rendering (no double buffering)
- `g_render_target` pointer either points to `g_backbuf` or `g_fb_base` (direct)

### 2. Explicit video_present() Model
- Caller-driven: rendering functions write to `g_render_target`, caller decides when to blit
- `video_present()` copies back buffer to framebuffer row-by-row via `mem_copy()`
- No-op when double buffering is inactive (direct rendering mode)
- All existing video functions (`framebuffer_store_pixel`, `framebuffer_fill_rect`, `scroll_up`) work through the indirection layer without code changes

### 3. Scanline Blitting
- `video_blit_row(dst_x, dst_y, pixels_rgb, count)` — blit an entire scanline in one call
- Uses `mem_copy32()` for 32bpp (4 bytes/pixel), per-pixel conversion for 16bpp
- Splash renderers (`splash_render_rgba_scaled`, `splash_render_ascii_luma_scaled`) accumulate pixels into a static scanline buffer, then blit once per row
- Reduces per-pixel function call overhead from ~786K to ~600 calls

### 4. GOP Mode Selection
- Enumerates all UEFI GOP modes at boot
- Preferred resolution list: 1024x768, 1280x720, 800x600, 1280x1024, 1920x1080 (32bpp only)
- Calls `SetMode` with best match; completely non-fatal on failure (uses default)
- Runs before framebuffer info is captured into `boot_info`/`handoff`

### 5. BSS Size Constraint
- Initial 2048x1080 buffer (~9MB) caused `AllocateAddress` failure in UEFI loader
- Final 800x600 buffer (~1.9MB) keeps total stage2 memsz at ~4.9MB (baseline was ~3.0MB)
- QEMU OVMF firmware has memory reservations that prevent allocating contiguous pages above ~8MB at the 3MB load address

## Validation

- `make clean && make` — zero errors, zero warnings
- `make test-stage2` — PASS (all markers found, no panics)
- `make test-gui-desktop` — PASS
- `make test-freedos-pipeline` — PASS
- `make test-opengem` — PASS
- `make test-fallback` — PASS

## Risks and Limitations

1. **Buffer too small for high-res**: If GOP mode selection picks 1024x768 or higher, the 800x600 buffer won't cover it → falls back to direct rendering (no double buffering benefit). Future work: dynamic buffer allocation when a page allocator is available.
2. **Present stale risk**: Any rendering path that doesn't call `video_present()` will leave stale content on screen. All current paths are covered; new features must remember to call it.
3. **No dirty-rect tracking**: `video_present()` blits the entire framebuffer every time. For partial updates (e.g., cursor blink), this is wasteful. Future optimization: dirty-rect or damage tracking.
4. **GOP mode selection runs before UEFI memory map**: If `SetMode` changes the memory map, the subsequent `ExitBootServices` should still work (it retries with fresh map). Tested successfully.

## Next Steps

1. **Increase buffer size** once stage2 has a page allocator or loads at a higher address
2. **Dirty-rect optimization** for partial screen updates
3. **VSync/page flip** if GOP supports it (rare in UEFI)
4. **Font rendering optimization** — batch glyph rendering into scanline buffers
