# HANDOFF - Boot HUD (Status Overlay)

## Date
`2026-04-16`

## Context
Task G2 from Codex: Add a minimal, readable HUD in graphical mode showing runtime status in real-time during boot.

## Completed scope
1. Added `ui_draw_boot_hud()` function to UI module (`stage2/include/ui.h`, `stage2/src/ui.c`).
2. Integrated HUD rendering into `stage2/src/stage2.c` `show_boot_splash()` function.
3. HUD displays: CiukiOS label, version string, boot mode (gfx/ascii), and progress bar.
4. Added serial marker `[ ui ] boot hud active` when HUD activates.
5. Updated `scripts/test_stage2_boot.sh` to assert HUD marker.

## Technical decisions
1. **HUD design**:
   - Compact panel (60px height) in top-left corner of graphical mode
   - Dark background (0x00101010U) with gray border (0x00505050U)
   - Bright green text (0x0000FF00U) for high contrast
   - Simple character-based progress indicator (# for progress, - for remaining)

2. **Rendering strategy**:
   - HUD drawn once at boot splash start
   - Updated on each progress percentage change
   - Uses existing UI primitives (ui_draw_panel, ui_write_centered_row, etc.)
   - Falls back gracefully if screen too small (< 320x240px)

3. **Lifecycle**:
   - HUD only visible during 2-second splash wait period
   - Progress updated from 0% to 100%
   - Marker printed to serial log exactly once when HUD becomes active

4. **Non-blocking safety**:
   - HUD rendering does not block boot sequence
   - No hidden state or side effects
   - Deterministic output (idempotent redraw)

## Touched files
1. `stage2/include/ui.h` (modified - added ui_draw_boot_hud declaration)
2. `stage2/src/ui.c` (modified - added ui_draw_boot_hud implementation, ~80 lines)
3. `stage2/src/stage2.c` (modified - integrated HUD into show_boot_splash)
4. `scripts/test_stage2_boot.sh` (modified - added HUD marker assertion)

## ABI/contract changes
None. Pure UI overlay addition.

## Tests executed
1. **make test-stage2**:
   Result: ✅ PASS
   - HUD marker asserted: `[OK] found: [ ui ] boot hud active`
   - All existing markers still present
   - No ticker flood or blocking loops

2. **make test-fallback**:
   Result: ✅ PASS
   - Kernel fallback boot unaffected

3. **make test-fat-compat**:
   Result: ✅ PASS (12/12 checks)

4. **Build verification**:
   Result: ✅ PASS
   - No warnings, clean compilation

## Current status
1. Boot HUD is operational and renders on graphical boot.
2. Progress updates during splash wait period.
3. Serial marker logs successfully on HUD activation.
4. Test assertions updated and passing.
5. All critical tests remain green.

## Risks / technical debt
1. Colors and layout are hardcoded - no theme system yet.
   - Acceptable for MVP; future refactor could add theme support.

2. Progress calculation is simple (linear 0-100 over 2s) - doesn't reflect actual boot phases.
   - Intentional: HUD is cosmetic overlay, not critical path feedback.

3. HUD assumes font metrics (8px character height) - not fully parameterized.
   - Acceptable constraint; could be enhanced if font system changes.

## Next steps (recommended order)
1. Merge G1 + G2 branches into main via Codex.
2. Consider adding HUD for shell startup phase (post-splash).
3. Add more detailed status messages to HUD if boot becomes longer.
4. Later: implement theme/color system for branded appearance.

## Notes for next agent
1. HUD is purely cosmetic - removing it would not affect boot functionality.
2. UI primitives make HUD trivial to customize (colors, layout, content).
3. Serial marker `[ ui ] boot hud active` is stable and testable.
4. HUD gracefully handles missing graphics (returns 0, no rendering).
5. Task G1 foundation enabled clean implementation of G2.
