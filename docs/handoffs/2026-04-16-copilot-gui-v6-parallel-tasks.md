# Handoff: GUI v6 Parallel Tasks (G6-G10)

**Date:** 2026-04-16
**Task source:** `docs/collab/parallel-next-tasks-2026-04-16-gui-v6.md`

## Task Summary

| Task | Branch | Status | Commit |
|------|--------|--------|--------|
| G6 - Exit chord ALT+G+Q | `feature/copilot-gui-exit-chord-v6` | DONE | 304875d |
| G7 - Dock text clipping | _covered by alignment surgical v6_ | DONE (prior) | e9e4367 |
| G8 - Window chrome padding | _covered by alignment surgical v6_ | DONE (prior) | e9e4367 |
| G9 - Footer responsive text | _covered by alignment surgical v6_ | DONE (prior) | e9e4367 |
| G10 - GUI regression gate v2 | `feature/copilot-gui-regression-gate-v2` | DONE | 8bf0e18 |

## G6 — Desktop Exit Chord ALT+G+Q

### Problem
Single ESC key exits the desktop session, making accidental exits too easy.

### What Changed
1. **keyboard.c**: Added `ALT_LEFT_BIT` tracking for scancode `0x38` (Left ALT make/break). New public function `stage2_keyboard_alt_held()`.
2. **keyboard.h**: Declared `stage2_keyboard_alt_held()`.
3. **shell.c**: Desktop loop now uses 2-stage chord: ALT held + G → stage 1, ALT held + Q → exit. Any non-chord key or ALT release resets. ESC no longer exits.
4. **ui.c**: Footer hint text updated from "ESC: shell" to "ALT+G+Q: Exit".

### Serial Markers
- `[ ui ] desktop exit chord alt+g+q active` — printed on desktop session start
- `[ ui ] exit chord alt+g+q triggered` — printed when chord completes

### Files Modified
- `stage2/src/keyboard.c` — ALT state tracking + public accessor
- `stage2/include/keyboard.h` — `stage2_keyboard_alt_held()` declaration
- `stage2/src/shell.c` — chord detector replaces ESC exit
- `stage2/src/ui.c` — footer hint text

## G7/G8/G9 — Already Covered

These three tasks were fully implemented in the alignment surgical v6 commit (branch `feature/copilot-gui-alignment-surgical-v6`, commit `e9e4367`):

- **G7** (dock clipping): `ui_draw_text_clipped()` with `~` truncation for dock labels
- **G8** (window chrome): `UI_TITLEBAR_H` (24px), centered title text, content rects with padding
- **G9** (footer responsive): 3-tier auto-shorten (64/40/<40 cols), clipped to status bar inner rect

See `docs/handoffs/2026-04-16-copilot-gui-alignment-surgical-v6.md` for full details.

## G10 — GUI Regression Gate v2

### Problem
Previous test script only had basic marker checks with no categorization or visual risk detection.

### What Changed
Rewrote `scripts/test_gui_desktop.sh` with:
1. **Three marker categories**: architecture (WARN), alignment (WARN), interactive (info)
2. **Negative pattern checks**: `[ panic ]`, `Invalid Opcode`, `#UD` → FAIL if found
3. **Layout zone debug display**: prints zone rects from log when available
4. **New markers checked**: `alignment surgical v6 active`, `desktop exit chord alt+g+q active`

### Files Modified
- `scripts/test_gui_desktop.sh` — full rewrite with v2 diagnostics

## Tests Executed

All branches tested against merge gate:

| Test | G6 Branch | G10 Branch |
|------|-----------|------------|
| `make test-stage2` | PASS | PASS |
| `make test-int21` | PASS | PASS |
| `make test-gui-desktop` | PASS | PASS |

## Residual Risks

| Risk | Mitigation |
|------|-----------|
| ALT+G+Q requires keyboard with working ALT scancode | Standard PS/2 set1 scancode; QEMU emulates correctly |
| Right ALT (AltGr, scancode E0 38) not tracked | Only Left ALT needed; right ALT has extended prefix, handled separately |
| Chord state reset on ALT release may feel unresponsive | Expected behavior — prevents ghost chords |

## Next Steps
1. Merge G6 and G10 branches
2. Verify all markers appear in interactive desktop session test
3. Consider adding CTRL modifier tracking for future commands
