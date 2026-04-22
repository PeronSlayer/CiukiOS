# HANDOFF - stage2 framebuffer font scaled 2x for readability

## Date
`2026-04-15`

## Context
User reported text readability issues in QEMU window and requested larger characters.

## Completed scope
1. Updated framebuffer text renderer to scale existing 8x8 glyphs by `2x` in both axes.
2. Kept existing bitmap font data unchanged (no glyph redesign needed).
3. Preserved all shell/UI logic (top bar, scrolling, commands) while increasing character size.

## Touched files
1. `stage2/src/video.c`

## Technical decisions
1. Decision: scale rasterization (draw-time) instead of replacing font assets.
   Reason: fastest safe path with minimal risk.
   Impact: text becomes visibly larger while keeping current ASCII coverage.

2. Decision: use `FONT_SCALE_X=2`, `FONT_SCALE_Y=2`.
   Reason: improves readability without reducing terminal columns too aggressively.
   Impact: effective cell size becomes 16x16.

## ABI/contract changes
1. None.

## Tests executed
1. `make test-stage2`
   Result: PASS
2. `make test-fallback`
   Result: PASS

## Current status
1. Shell text and title are now larger and easier to read.
2. Boot/fallback regressions remain green.

## Risks / technical debt
1. Font remains pixel-doubled 8x8 bitmap (not a true higher-detail font).
2. Fewer columns/rows are available on screen due to larger cell size.

## Next steps (recommended order)
1. Optional: make font scale configurable via compile-time macro.
2. Optional: add a real 8x16 or 12x24 bitmap font set for sharper readability.

## Notes for Claude Code
- Scaling is implemented in `draw_char()` by replicating each glyph pixel in X/Y loops.
- Constants introduced: `GLYPH_W/H`, `FONT_SCALE_X/Y`, with derived `FONT_W/H`.
