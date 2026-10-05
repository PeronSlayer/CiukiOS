# Dynamic Window Titles and Cursor Schemes Validation (2026-10-05)

## 1. Context & Motivation

1. **Window Title Overflow**:
   In previous builds, `ui_title_rail` in `src/com/shell_gui_draw.inc` had a hardcoded title tab width of 180 pixels (`mov cx, 180`) and hardcoded rail offsets (`add bx, 188; sub cx, 258`). When a window had a long document name (such as `New Text Document.txt - CiukNote`, ~240 px wide), the indigo title background stopped prematurely at 180 px while the text continued over the platinum pinstripes. Because title captions are drawn in white (`0x010F`), the overflowing characters became unreadable over the light grey pinstripes.
2. **Text Field Cursor & Caret Ergonomics**:
   - Caret blinking in text inputs (CiukNote, Browser, dialogs) was static.
   - Mouse hover over editable text did not indicate text selection (`CURSOR_IBEAM`), and non-editable or disabled inputs lacked visual feedback (`CURSOR_FORBIDDEN`).
   - The system lacked cursor theme options and a settings interface in Control Panel.

## 2. Implementation

### 2.1 Dynamic Window Title Rail (`src/com/shell_gui_draw.inc`)
- `ui_title_rail`:
  - Renders the base platinum (color 7) background across the full window title geometry.
  - Draws the platinum pinstripes (`.rail`) across the entire rail zone up to the reserved window controls area (`cx - 80`).
- `ui_title_text`:
  - Measures the exact proportional text width (`uit_text_width`), accounting for truncation with ellipsis when bounded by window width (`uit_max`).
  - Dynamically calculates the tab width: `tab_w = max(uit_text_width + 19, 96)` bounded before window controls (`uit_max + 12`).
  - Renders the solid active indigo (`al=1`) or inactive slate (`al=8`) tab rectangle over the rails.
  - Clears a 6-pixel clean platinum spacer between the tab edge and the pinstripes.
  - Renders the white caption text centered within the tab with generous padding, preventing text spillover across all window sizes and screen resolutions.

### 2.2 Cursor Themes & Control Panel Applet
- **Theme Asset**:
  - `scripts/build_cursors.py` generates `assets/cursors/CURSORS.DAT` (654 bytes) with 3 schemes:
    1. Tango (Default, Public Domain)
    2. Classic 95
    3. 3D Contrast
  - Each scheme includes Arrow, I-Beam, and Forbidden cursors with 64-byte masks and 2-byte hotspots.
- **Kernel & Shell Integration**:
  - `src/apps/app.h`: added `CURSOR_ARROW (0)`, `CURSOR_IBEAM (1)`, `CURSOR_FORBIDDEN (2)`, `ui_cursor(type)`, expanded `struct deskcfg` with `cursor_scheme`.
  - `src/com/shell_gui_input.inc`: hotspot subtraction during pointer drawing (`ui_pointer_show`, `ui_vga_pointer_show`), active scheme loader `ui_set_cursor_scheme` and dynamic apply `ui_cursor_apply`.
  - `src/com/shell_apps.inc`: service 30 dispatch `app_s_cursor`, auto-reset to Arrow on `app_hover_leave`.
- **Control Panel**:
  - `src/apps/control.c`: added "Pointer scheme" group to Mouse applet (`P_MOUSE`) with radio buttons and live interactive preview box.
  - Saves choice to byte 72 of `\SYSTEM\UI\DESKTOP.CFG`.
- **Caret Blinking & Hover**:
  - `src/apps/ui.c`, `src/apps/ciuknote.c`, `src/apps/browser.c`: caret blinking driven by system timer ticks (`HOST.ticks / 9 & 1`).
  - Text fields and editor viewports trigger `ui_cursor(CURSOR_IBEAM)` on hover.

## 3. Verification & Results

1. **Targeted QEMU Boot**:
   - `scripts/tests/test_title_and_mouse.py` booted the fresh FAT16 HDD image (`build/full/ciukios-full.img`) with QEMU KVM.
2. **Visual Inspection**:
   - `docs/screenshots/0.8.3/window-title.png`: verified that long titles (`ongocumentitleest.txt - CiukNote`) expand the indigo tab to completely contain the title text without clipping or overflowing onto the pinstripes.
   - `docs/screenshots/0.8.3/mouse-scheme.png`: verified the Mouse applet layout with "Pointer scheme" radio group, "Classic 95" selected, and live preview box.
3. **Persistence**:
   - Verified that saving from the Mouse applet writes `cursor_scheme = 1` into byte 72 of `::SYSTEM/UI/DESKTOP.CFG` (74 bytes total).
