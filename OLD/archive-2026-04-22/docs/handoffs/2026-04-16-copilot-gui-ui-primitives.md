# HANDOFF - UI Primitives Module (Stage2)

## Date
`2026-04-16`

## Context
Task G1 from Codex: Create a reusable Stage2 GUI utility layer so rendering logic is no longer scattered in `stage2.c`. This enables cleaner composition for future HUD components.

## Completed scope
1. Created `stage2/include/ui.h` - public interface for UI primitives.
2. Created `stage2/src/ui.c` - implementation of rendering helpers.
3. Refactored `stage2/src/stage2.c` to use new UI primitives.
4. Preserved all existing visual behavior (no regressions).

## Technical decisions
1. **Primitive set**:
   - `ui_draw_top_bar()` - horizontal top bar with centered text (text mode)
   - `ui_write_centered_row()` - centered text at row (text mode)
   - `ui_draw_progress_bar()` - filled progress bar (graphical mode)
   - `ui_draw_panel()` - rectangular frame/panel (graphical mode)
   - `ui_draw_separator_line()` - horizontal line (graphical mode)
   - `ui_pixel_y_to_text_row()` - coordinate converter utility

2. **Refactoring approach**:
   - Extracted `draw_splash_footer()` logic to use `ui_draw_panel()` + `ui_draw_progress_bar()`
   - Replaced `draw_title_bar()` local implementation with `ui_draw_top_bar()`
   - Removed `local_strlen()` and `video_write_centered_row()` statics from stage2.c
   - Centralized in ui.c for reuse

3. **Backward compatibility**:
   - All functions match existing visual parameters
   - Colors, positions, dimensions preserved exactly
   - No ABI/handoff changes

## Touched files
1. `stage2/include/ui.h` (new - 81 lines, public interface)
2. `stage2/src/ui.c` (new - 173 lines, implementation)
3. `stage2/src/stage2.c` (modified - removed statics, added ui.h include, refactored rendering calls)

## ABI/contract changes
None. Pure utility layer refactoring.

## Tests executed
1. **make test-stage2**:
   Result: ✅ PASS
   - All critical markers found
   - No regressions in boot sequence

2. **make test-fallback**:
   Result: ✅ PASS
   - Kernel boot unaffected

3. **Build verification**:
   Result: ✅ PASS
   - ui.c compiles without warnings
   - stage2.o links successfully

## Current status
1. UI primitives module is operational.
2. Rendering logic is now centralized and reusable.
3. Boot splash and title bar rendering work identically (visual regression tests pass).
4. Ready for Task G2 (Boot HUD) to build on this foundation.

## Risks / technical debt
1. Pixel-to-text-row converter (`ui_pixel_y_to_text_row()`) assumes 8px character height - hardcoded.
   - Future: could parameterize based on font metrics if needed.

2. Color scheme is currently hardcoded in calling code - no theme system yet.
   - This is intentional for MVP - preserved existing behavior.

## Next steps (recommended order)
1. Implement Task G2 (Boot HUD) using these primitives.
2. Consider adding `ui_draw_text_box()` for message dialogs.
3. Later: implement theme/color management system.

## Notes for next agent
1. UI module is header-only + implementation pattern - easy to extend.
2. All primitives check `video_ready()` before drawing - safe to call.
3. Functions are deterministic and stateless - no hidden state.
4. Parameters are in consistent order: coordinates first, then sizes, then colors/options.
5. Ready to be called from multiple contexts (boot, shell, HUD).
