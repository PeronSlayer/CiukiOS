# HANDOFF - stage2 splash readability fix (dynamic font scale)

## Context
User reported splashscreen visible but poorly due to large characters. Requirement: render splash much smaller while keeping shell readability.

## What Changed
1. Added runtime font scaling to video subsystem:
   - New API: `video_set_font_scale(scale_x, scale_y)`
   - Added helpers for dynamic glyph metrics and viewport recomputation.
2. Converted video rendering path from compile-time fixed 2x scale to runtime scale:
   - `draw_char`, `scroll_up`, clear logic now use dynamic `font_w()/font_h()`.
3. Boot flow updated:
   - Splash now renders in `1x1` font scale.
   - After splash timeout/key skip, scale is restored to `2x2`.
   - Title bar is drawn after splash restore so shell stays readable.

## Files Modified (this step)
1. `stage2/include/video.h`
2. `stage2/src/video.c`
3. `stage2/src/stage2.c`

## Behavioral Result
- Splash: high density (`1x1`), much more detail visible.
- Shell/runtime UI: preserved readability (`2x2`).
- No changes to shell command semantics.

## Validation
Executed:
1. `make test-stage2` -> PASS
2. `make test-fallback` -> PASS

## Notes
- Current implementation supports scale range `[1..4]` and clamps values.
- `video_set_font_scale` currently clears framebuffer and resets text window to full screen; caller is responsible for redrawing UI layers (title bar, etc.).
