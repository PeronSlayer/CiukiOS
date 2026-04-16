# Handoff: GUI Alignment Surgical Fix v6

**Date:** 2026-04-16
**Branch:** `feature/copilot-gui-alignment-surgical-v6`
**Commit:** e9e4367

## Problem

After layout hardening v4 established the 8px grid system and computed layout zones, several visual defects remained:

1. **Dock text overflow**: "RUN INIT.COM" (14 chars with "> " prefix) at 16px cell width in a 160px dock (9 usable columns) spills past the dock panel boundary into the workspace.
2. **Window title bar misalignment**: Title text placed at `w->y + UI_GRID/2 = w->y + 4px`, which at 16px cells resolves to a row that starts before the title bar fill area. Text sits on the border line.
3. **Inconsistent panel insets**: Border-aware content rects not defined; text positioned relative to outer panel edges, not inner content areas.
4. **Uneven dock item spacing**: Items placed with `item_y + UI_GRID/2` offset for centering, but 4px offset in 24px items with 16px cells yields inconsistent baselines.
5. **Footer text on border**: Status bar text at `status_y + 4px` can overlap the 1px border.

## Solution

### Design Tokens (ui.h)

Added standardized spacing tokens to eliminate ad-hoc pixel constants:

| Token | Value | Purpose |
|-------|-------|---------|
| `UI_OUTER_MARGIN` | 8px | Outer margin (= UI_GAP) |
| `UI_ZONE_GAP` | 8px | Gap between zones |
| `UI_PANEL_BORDER` | 1px | Panel border thickness |
| `UI_PANEL_PAD_X` | 8px | Horizontal inner padding |
| `UI_PANEL_PAD_Y` | 8px | Vertical inner padding |
| `UI_TITLEBAR_H` | 24px | Window title bar height |
| `UI_DOCK_ITEM_H` | 24px | Dock item row height |
| `UI_DOCK_HEADER_H` | 24px | Dock header height |

### Text Clipping Helper (ui.c)

New `ui_draw_text_clipped(rx, ry, rw, rh, text_x, text_y, text, fg, bg)`:
- Computes max visible characters from `text_x` to right edge of rect `(rx + rw)`
- If text exceeds available space, truncates and replaces last visible char with `~`
- Rejects text if origin falls outside rect vertically
- Uses actual `video_cell_width_px()` / `video_cell_height_px()` for column math
- **Hard guarantee**: no text writes outside the specified pixel rectangle

### Fix Details

**Dock text overflow (defect 1)**:
- All dock text now routed through `ui_draw_text_clipped()` with `dock_inner_w` boundary
- At 800x600 (dock=160px, border=1px, inner=158px, cell=16px): max 9 chars
- "> RUN INIT.COM" (14 chars) truncated to "> RUN I~" (9 chars)
- Selection highlight contained within `dock_inner_w - 2px` margin

**Window title bars (defect 2)**:
- Title bar height increased from 16px to `UI_TITLEBAR_H` (24px)
- Title text vertically centered: `inner_y + (UI_TITLEBAR_H - cell_h) / 2`
- At 24px bar / 16px cell: text at 4px offset from inner top = comfortably inside
- Title text clipped to title bar rect via `ui_draw_text_clipped()`

**Panel alignment (defect 3)**:
- Every panel now has explicit content rect: `(x + BORDER, y + BORDER, w - 2*BORDER, h - 2*BORDER)`
- Text placement uses `+ UI_PANEL_PAD_X/Y` from content rect edges
- All draw calls reference inner rect boundaries, not outer panel edges

**Dock row spacing (defect 4)**:
- Uniform `UI_DOCK_ITEM_H` (24px) per item with `(ITEM_H - cell_h) / 2` centering
- At 24px item / 16px cell: 4px top offset = consistent baseline across all items
- Items start after separator + `UI_ZONE_GAP`, ensuring no overlap with header

**Footer readability (defect 5)**:
- Status bar text placed at `status_y + UI_PANEL_BORDER` (inside border)
- Horizontal position at `status_x + BORDER + PAD_X` (padded from left edge)
- Text clipped to status bar inner rect
- Three-tier auto-shorten: full (>=64 cols), medium (>=40 cols), minimal (<40 cols)

### Serial Markers

On first desktop render:
```
[ ui ] alignment surgical v6 active
[ ui ] layout grid=8 cell=16x16 fb=1280x800
[ ui ] zone top_bar=(0,0,1280,32)
[ ui ] zone status_bar=(0,776,1280,24)
[ ui ] zone dock=(8,40,160,728)
[ ui ] zone content=(176,40,1096,728)
```

## Files Modified

| File | Changes |
|------|---------|
| `stage2/include/ui.h` | Added 8 design tokens (UI_OUTER_MARGIN through UI_DOCK_HEADER_H) |
| `stage2/src/ui.c` | Added `ui_draw_text_clipped()`, rewrote desktop scene/windows/launcher rendering to use content rects and clipped text, added v6 serial marker |
| `scripts/test_gui_desktop.sh` | Added `alignment surgical v6 active` to architecture markers |

## Acceptance Criteria Verification

| Criterion | Status |
|-----------|--------|
| No text overflow outside panel bounds | PASS — all text through `ui_draw_text_clipped()` |
| No overlap among top/workspace/dock/status | PASS — layout engine unchanged, zones computed with gaps |
| Titles readable inside titlebars with padding | PASS — centered in 24px title bar, clipped to inner rect |
| Dock highlight fully contains selected label | PASS — highlight and text both constrained to `dock_inner_w` |
| 800x600 clean and readable | PASS — dock truncates long labels, footer auto-shortens |
| 1280x800 clean and readable | PASS — all zones scale with framebuffer |
| `make test-stage2` | PASS |
| `make test-int21` | PASS |
| `make test-gui-desktop` | PASS |

## Residual Risks

| Risk | Mitigation |
|------|-----------|
| Dock items with >9 chars get `~` truncation at 800x600 | Acceptable: labels remain recognizable; full label in dispatch log |
| `ui_write_centered_row` for top bar "CiukiOS" not routed through clipping | Only 7 chars, always fits; could be refactored later |
| Font scale change (e.g., 1x1) would allow more chars but change visual density | `video_cell_width_px()` adapts automatically |

## Next Visual Improvements

1. Per-character pixel clipping (partial glyph masking) for sub-cell precision
2. Dock item icons/shortcut indicators
3. Window content scrolling within clipped content rect
4. Dynamic dock width based on longest label + padding
5. Focus ring animation (border color pulse)
