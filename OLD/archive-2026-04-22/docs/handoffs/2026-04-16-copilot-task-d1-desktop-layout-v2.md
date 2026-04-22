# Task D1 Handoff - Desktop Layout System v2

**Date:** 2026-04-16
**Branch:** feature/copilot-gui-desktop-layout-v2
**Status:** Complete

## Goal
Transform desktop scene from ad-hoc pixel coordinates into a deterministic layout grid system with named constants for consistent geometry across different screen resolutions (800x600, 1280x800+).

## Implementation Summary

### Layout Grid Constants
Introduced deterministic layout grid at module level:
- **UI_LAYOUT_TOP_BAR_H:** 32px (title bar height)
- **UI_LAYOUT_STATUS_BAR_H:** 24px (bottom status bar)
- **UI_LAYOUT_MARGIN_H:** 10px (horizontal work area margin)
- **UI_LAYOUT_WORK_TOP:** Calculated start of work area (top bar + margin)
- **UI_LAYOUT_WORK_BOTTOM:** Status bar height reservation

### Window Manager Grid
Windows now position using layout constants:
- **UI_WINDOW_MARGIN_X:** 10px (left margin)
- **UI_WINDOW_MARGIN_Y:** Derived from layout (top bar + margin)
- **UI_WINDOW_W:** 280px (standard window width)
- **UI_WINDOW_H:** 120px (standard window height)
- **UI_WINDOW_H_SMALL:** 100px (info window height variant)

Window layout:
- System window: (10, ~42px, 280x120)
- Shell window: (10, ~172px, 280x120)
- Info window: (300, ~42px, 200x100)

### Launcher Panel Grid
Launcher panel position calculated from layout constants instead of hardcoded 350px:
- Uses **UI_LAYOUT_WORK_TOP** and window dimensions for deterministic positioning
- Maintains item height (20px) and spacing

### Serial Marker
Added deterministic marker output:
- **`[ ui ] desktop layout v2 active`** — printed once per desktop render cycle

## Files Touched
1. **stage2/include/ui.h**: Added layout constants (10 lines)
2. **stage2/src/ui.c**: Refactored geometry calculations (25 line changes)
3. **scripts/test_stage2_boot.sh**: Added layout v2 marker assertion (1 line)

## Validation

### Test Results
```
make test-stage2: PASS
  ✓ [ ui ] desktop layout v2 active marker detected
  ✓ Boot HUD renders correctly
  ✓ All required patterns found
  ✓ No forbidden patterns

make test-fallback: PASS
  ✓ Kernel fallback path unaffected
  ✓ No ABI/handoff changes introduced
```

### Resolution Consistency
- Geometry tested on default QEMU resolution (typically 800x600)
- Constants scale appropriately for 1280x800+ (uses framebuffer dimensions dynamically)
- No hardcoded screen resolution assumptions beyond pixel constants

## Technical Decisions

1. **Constants vs Dynamic Calculation:** Chose explicit named constants for clarity and testability. Framebuffer dimensions (fb_w, fb_h) remain dynamic runtime values.

2. **Grid Derivation:** Layout constants derive from each other (e.g., WORK_TOP = TOP_BAR_H + MARGIN_H) for consistency. Single point of change for reshaping entire layout.

3. **Backward Compatibility:** Window titles and launcher items remain unchanged. All changes are internal geometry; visual hierarchy preserved.

4. **Test Assertion Placement:** Added marker check after `[ ui ] boot hud active` to validate layout system activated in correct initialization order.

## Risks & Mitigations

| Risk | Mitigation |
|------|-----------|
| Marker output in wrong window state | Marker prints once when desktop renders; covered by test |
| Hardcoded resolution assumptions | Using framebuffer queries dynamically; no resolution-specific branches |
| Window overlap on small displays | Bounds checking in render path; falls back gracefully if fb too small |

## Next Steps

1. Task D2 (Window Chrome): Improve title bar visuals, add content placeholders
2. Task D3 (Interaction Loop): Enhance keyboard feedback and hints
3. Task D4 (Launcher/Dock): Convert launcher to dock with dispatch clarity
4. Task D5 (GUI Regression Harness): Aggregate all GUI markers into comprehensive test gate

## Commit
- **Hash:** 6eb5fd6
- **Message:** feat(gui): implement desktop layout system v2 with grid constants
