# Handoff - Splash Footer (Version + Loading Bar) (2026-04-15)

## Objective
Add boot-time UI elements under the PNG splash:

1. Display current OS version.
2. Display a loading progress bar.

## What was implemented

### 1) Splash layout reservation for footer
- Added new API in `splash` module:
  - `stage2_splash_show_graphic_layout(u32 reserved_bottom_px)`
- Existing `stage2_splash_show_graphic()` now delegates to layout API with `0` reserved pixels.
- Graphic/ASCII render path in `splash.c` now supports rendering inside a target area (top area), leaving bottom space for UI overlay.

### 2) Boot footer in Stage2
- `show_boot_splash()` now:
  - sets font scale to `1x1` during splash phase,
  - computes a dynamic footer height,
  - renders PNG splash with reserved bottom area,
  - draws footer with:
    - centered version string (`CiukiOS Stage2 v0.5`),
    - centered `Loading...` label,
    - progress bar updated over splash wait ticks.
- Progress updates each loop iteration based on elapsed ticks (`0..100%`).

### 3) Unified version string
- Added `stage2/include/version.h`:
  - `CIUKIOS_STAGE2_VERSION`
  - `CIUKIOS_STAGE2_VERSION_LINE`
- `shell ver` now prints version from this shared header to avoid drift.

## Files changed

- `stage2/include/splash.h`
- `stage2/src/splash.c`
- `stage2/src/stage2.c`
- `stage2/include/version.h` (new)
- `stage2/src/shell.c`

## Validation

All regression tests passed:

- `make test-stage2`
- `make test-fallback`
- `make test-fat-compat`

## Notes

- Footer UI is only drawn in graphic splash mode.
- ASCII fallback path remains unchanged and safe.
- After splash, runtime restores shell font/layout (`2x2` and title bar flow).
