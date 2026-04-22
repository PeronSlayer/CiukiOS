# Copilot Desktop Polish Roadmap (2026-04-16)

Goal:
- Turn the current desktop scene from "debug blocks" into a readable, coherent, testable GUI shell.
- Keep deterministic boot/runtime behavior and preserve DOS roadmap velocity.

Hard constraints:
1. No loader/boot handoff ABI changes.
2. No regressions on `test-stage2`, `test-fallback`, `test-int21`.
3. Do not revert unrelated Codex/Claude changes.
4. Keep serial markers deterministic and grep-friendly.

## Task D1 - Desktop Layout System v2
Branch:
- `feature/copilot-gui-desktop-layout-v2`

Scope:
- `stage2/include/ui.h`
- `stage2/src/ui.c`

Implement:
1. Introduce a deterministic layout grid (margins, top bar, work area, status/dock area).
2. Replace ad-hoc pixel constants with named layout constants.
3. Add one serial marker when layout v2 is active:
   - `[ ui ] desktop layout v2 active`

Acceptance:
1. `make test-stage2` PASS
2. Desktop scene renders consistent geometry across 800x600 and 1280x800.

## Task D2 - Window Chrome and Readability Pass
Branch:
- `feature/copilot-gui-window-chrome-v2`

Scope:
- `stage2/src/ui.c`
- `stage2/include/ui.h`

Implement:
1. Improve window visuals (title bar, border hierarchy, active/inactive contrast).
2. Add deterministic content placeholders in each window (e.g. "System", "Shell", "Info").
3. Ensure text placement is readable and not overlapping random blocks.
4. Add marker:
   - `[ ui ] window chrome v2 ready`

Acceptance:
1. `make test-stage2` PASS
2. Window focus state is visually obvious.

## Task D3 - Desktop Interaction Loop Upgrade
Branch:
- `feature/copilot-gui-desktop-interaction-v2`

Scope:
- `stage2/src/shell.c`
- `stage2/src/ui.c`
- `stage2/include/ui.h`
- `stage2/src/keyboard.c` (only if needed)

Implement:
1. Keep current desktop session loop, but improve interaction feedback:
   - focused window indicator,
   - selected launcher item indicator,
   - lightweight on-screen hints.
2. Add key handling for both arrows and WASD/JK fallback where practical.
3. Add marker on first interaction event:
   - `[ ui ] desktop interaction active`

Acceptance:
1. `make test-stage2` PASS
2. `desktop` command remains fully usable with keyboard only.

## Task D4 - Launcher/Dock Visual + Dispatch Clarity
Branch:
- `feature/copilot-gui-launcher-dock-v2`

Scope:
- `stage2/src/ui.c`
- `stage2/include/ui.h`
- `stage2/src/shell.c`

Implement:
1. Convert launcher panel into clear dock/menu region with explicit selected item state.
2. Improve dispatch UX (show selected command label before dispatch).
3. Preserve existing command bridge behavior (no duplicate business logic).
4. Add marker per dispatch:
   - `[ ui ] launcher dispatch v2`

Acceptance:
1. `make test-stage2` PASS
2. `make test-fat-compat` PASS

## Task D5 - GUI Regression Harness v2
Branch:
- `feature/copilot-gui-regression-v2`

Scope:
- `scripts/test_stage2_boot.sh`
- `scripts/` (new gui check script if useful)
- `Makefile` (optional target)
- `docs/handoffs/`

Implement:
1. Extend stage2 test assertions with the new GUI markers from D1-D4.
2. Add optional `make test-gui-desktop` helper for focused GUI marker checks.
3. Keep full pipeline deterministic and green.

Acceptance:
1. `make test-stage2` PASS
2. `make test-int21` PASS
3. `make test-fallback` PASS
4. `make test-fat-compat` PASS
