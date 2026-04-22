# HANDOFF - Scene Core + Desktop Entry

## Date
`2026-04-16`

## Context
Task 1 GUI Roadmap: Introduce a minimal scene system for Stage2 to switch between boot splash and desktop environments.

## Completed scope
1. Added scene enum (`SCENE_BOOT_SPLASH`, `SCENE_DESKTOP`) in ui.h
2. Scene state management functions: `ui_get_scene()`, `ui_set_scene()`, `ui_render_scene()`
3. Desktop entry point: `ui_enter_desktop_scene()` with serial marker
4. Placeholder desktop renderer (empty, ready for Task 2)
5. Shell command hook: `desktop` command triggers scene switch

## Touched files
1. `stage2/include/ui.h` - Added scene enum and function declarations
2. `stage2/src/ui.c` - Implemented scene management + placeholder renderer
3. `stage2/src/shell.c` - Added ui.h include, added `desktop` command

## Tests executed
- ✅ `make test-stage2` PASS
- ✅ `make test-fallback` PASS

## Technical decisions
1. **Scene as state machine**: Simple enum-based dispatch, no complex callbacks
2. **Placeholder renderer**: Empty desktop renderer ready for Task 2 implementation
3. **Serial marker**: `[ ui ] scene=desktop` logs exactly once on desktop entry
4. **No blocking**: Scene switch is instant, non-blocking

## Current status
- Scene system operational and testable
- Boot path unchanged (still reaches shell)
- Desktop command available from shell prompt
- Ready for Task 2 (desktop shell surface)

## Risks / Next steps
- Desktop renderer is a stub (to be filled in Task 2)
- No visual feedback yet in desktop scene
