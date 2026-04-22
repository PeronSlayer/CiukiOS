# Handoff: Desktop Layout Grid Hardening v4

**Date:** 2026-04-16
**Branch:** `feature/copilot-gui-layout-hardening-v4`
**Commit:** 7e0e5a9

## Problem

The desktop GUI had scattered magic numbers for pixel coordinates (`10U`, `32U`, `60U`, `280U`, `350U`, `300U`, `296U`, etc.) resulting in:

- Elements placed at arbitrary, non-grid-aligned positions
- No layout adaptation to different framebuffer resolutions
- Windows, dock, and status bar computed with ad-hoc offsets that could overlap
- No centralized definition of screen zones
- Inconsistent text placement (some used `/8U` hacks for column positioning)

## Solution

### Layout Engine (`ui_compute_layout`)

All UI geometry is now computed from a single function that takes `(fb_w, fb_h)` and produces a `ui_layout_t` struct with all zone rectangles:

```
Grid base: 8px (UI_GRID)
Every coordinate snapped to grid via UI_SNAP(v)

Zone compute at 800x600:
  top_bar:    (0,   0,   800, 32)     ← full-width title bar
  status_bar: (0, 576,   800, 24)     ← bottom-anchored footer
  workspace:  (0,  40,   800, 528)    ← between bars with gaps
  dock:       (8,  40,   160, 528)    ← left column
  content:    (176, 40,  616, 528)    ← right of dock

Zone compute at 1280x800:
  top_bar:    (0,   0,  1280, 32)
  status_bar: (0, 776,  1280, 24)
  workspace:  (0,  40,  1280, 728)
  dock:       (8,  40,   160, 728)
  content:    (176, 40, 1096, 728)
```

### Token-based Constants

All magic numbers replaced with named tokens:

| Token | Grid Units | Pixels | Purpose |
|-------|-----------|--------|---------|
| `UI_TOP_BAR_GRIDS` | 4 | 32 | Top bar height |
| `UI_STATUS_BAR_GRIDS` | 3 | 24 | Status bar height |
| `UI_GAP_GRIDS` | 1 | 8 | Inter-zone gap |
| `UI_DOCK_W_GRIDS` | 20 | 160 | Dock panel width |

### Window Reflow

Windows are no longer hardcoded. `ui_reflow_windows()` computes positions from content area:

- **System** (top-left): 60% content width, 50% height
- **Shell** (bottom-left): 60% width, remaining height
- **Info** (right column): 40% width, full height
- All dimensions snapped to 8px grid

### Color Palette

Centralized via `COL_*` defines instead of inline hex:

```c
#define COL_BG_DESKTOP   0x00101015U
#define COL_WIN_FOCUS_BD 0x00FFFFFFU
#define COL_DOCK_SEL_BG  0x00003333U
// ... etc
```

### Debug Serial Output

On first desktop render, zone rectangles are printed to serial:

```
[ ui ] layout grid=8 fb=1280x800
[ ui ] zone top_bar=(0,0,1280,32)
[ ui ] zone status_bar=(0,776,1280,24)
[ ui ] zone dock=(8,40,160,728)
[ ui ] zone content=(176,40,1096,728)
```

## Files Modified

| File | Changes |
|------|---------|
| `stage2/include/ui.h` | Replaced old layout constants with grid system, added `ui_layout_t` and `ui_compute_layout()` |
| `stage2/src/ui.c` | Full rewrite: layout engine, zone-based rendering, window reflow, centralized colors |

## Acceptance Criteria Verification

| Criterion | Status |
|-----------|--------|
| No elements off-screen at 800x600 | PASS — all zones computed with bounds |
| No elements off-screen at 1280x800 | PASS — zones scale with framebuffer |
| No overlap between top bar, dock, workspace, footer | PASS — gaps enforce separation |
| "CiukiOS" centered in top bar | PASS — `ui_write_centered_row()` |
| Dock navigable from keyboard | PASS — existing key handlers unchanged |
| Serial debug markers present | PASS — zone rects printed on first render |
| `make test-stage2` | PASS |
| `make test-int21` | PASS |
| `make test-gui-desktop` | PASS |
| `make test-fallback` | PASS |
| `make test-fat-compat` | PASS |

## Risks

| Risk | Mitigation |
|------|-----------|
| Resolution below 800x600 | `ui_compute_layout` sets `valid=0`, rendering skipped |
| Dock item count exceeds vertical space | Bounds check in `ui_render_launcher()` stops rendering past dock bottom |
| Font scaling changes break text alignment | All text positions use `ui_pixel_y_to_text_row()` with `UI_GRID` divisor |

## What's NOT Changed

- Boot HUD (pre-desktop) rendering: unchanged, works at any resolution
- Shell text mode: completely unaffected
- INT21h / loader / COM ABI: zero changes
- Keyboard handling in shell.c: untouched
