# Copilot GUI Roadmap (2026-04-16)

Goal:
- Evolve Stage2 from boot-only graphics into a lightweight desktop-style graphical shell.
- No brand emulation and no external references: only CiukiOS visual identity.

Principles:
1. Keep boot/runtime deterministic.
2. No loader/handoff ABI changes.
3. Each step must preserve existing regression suite.
4. Add stable serial markers for GUI phases.

## Phase Overview
1. R1: Scene core (boot scene + desktop scene switch)
2. R2: Desktop shell surface (top bar, workspace, status line)
3. R3: Window manager baseline (window structs + focus cycle)
4. R4: Launcher + command bridge
5. R5: GUI regression harness and markers

## Task 1 - Scene Core + Desktop Entry
Branch:
- `feature/copilot-gui-desktop-scene-core`

Goal:
- Introduce a minimal scene system and allow entering a desktop scene after splash.

Scope (owned files):
- `stage2/include/ui.h`
- `stage2/src/ui.c`
- `stage2/src/stage2.c`
- `stage2/src/shell.c` (only command hook if needed)

Deliverables:
1. Add scene enum/state (`BOOT_SPLASH`, `DESKTOP`).
2. Add desktop renderer entry point (even if static first version).
3. Optional command `desktop` from shell to re-enter desktop scene.
4. Add serial marker:
   - `[ ui ] scene=desktop`

Acceptance:
1. `make test-stage2` PASS
2. `make test-fallback` PASS
3. Stage2 still reaches shell loop

## Task 2 - Desktop Shell Surface
Branch:
- `feature/copilot-gui-desktop-shell-surface`

Goal:
- Build a coherent desktop-like base layout using existing framebuffer primitives.

Scope (owned files):
- `stage2/src/ui.c`
- `stage2/include/ui.h`
- `stage2/src/stage2.c`

Deliverables:
1. Draw desktop background pattern/gradient.
2. Draw persistent top bar with centered `CiukiOS` and right-aligned version.
3. Draw bottom status strip with hint text (hotkeys/help).
4. Add serial marker:
   - `[ ui ] desktop shell surface active`

Acceptance:
1. `make test-stage2` PASS
2. No text artifacts over shell prompt area
3. No IRQ/tick flood regressions

## Task 3 - Window Manager Baseline (Keyboard-driven)
Branch:
- `feature/copilot-gui-window-manager-kbd`

Goal:
- Add a minimal internal window model and deterministic focus switching.

Scope (owned files):
- `stage2/include/ui.h`
- `stage2/src/ui.c`
- `stage2/src/keyboard.c` (only if strictly required)

Deliverables:
1. Define simple `ui_window_t` array (title, x/y, w/h, focused).
2. Render at least 2 mock windows in desktop scene.
3. Implement focus cycle by keyboard shortcut (for example `Tab`).
4. Focused window visual differentiation.
5. Add serial marker:
   - `[ ui ] wm focus cycle ok`

Acceptance:
1. `make test-stage2` PASS
2. Focus cycle deterministic and non-blocking
3. No changes to interrupt wiring

## Task 4 - Launcher Panel + Command Bridge
Branch:
- `feature/copilot-gui-launcher-panel`

Goal:
- Create a launcher area to trigger existing shell commands from GUI selections.

Scope (owned files):
- `stage2/src/ui.c`
- `stage2/include/ui.h`
- `stage2/src/shell.c`

Deliverables:
1. Launcher panel with 4-6 items (e.g. `DIR`, `MEM`, `CLS`, `VER`, `ASCII`, `RUN INIT.COM`).
2. Keyboard navigation in launcher (arrows + enter, or deterministic equivalent).
3. Bridge selected launcher item to existing shell command handlers.
4. Add serial marker:
   - `[ ui ] launcher command dispatched`

Acceptance:
1. `make test-stage2` PASS
2. `make test-fat-compat` PASS
3. Commands execute with existing semantics (no duplicates of logic)

## Task 5 - GUI Regression Gate
Branch:
- `feature/copilot-gui-regression-gate`

Goal:
- Lock GUI behavior with deterministic markers and tests.

Scope (owned files):
- `scripts/test_stage2_boot.sh`
- `scripts/` (new optional GUI check script)
- `Makefile` (new target if needed)
- `docs/handoffs/`

Deliverables:
1. Assert all GUI markers from tasks 1-4 in test path.
2. Optional `make test-gui-stage2` target for GUI-specific checks.
3. Keep current baseline tests green.

Acceptance:
1. `make test-stage2` PASS
2. `make test-int21` PASS
3. `make test-freedos-pipeline` PASS

## Merge Rules for Copilot
1. One task per branch, merge in order (Task1 -> Task5).
2. One handoff file per task: `docs/handoffs/YYYY-MM-DD-<topic>.md`.
3. Do not touch loader ABI or FreeDOS policy files in this cycle.
4. If conflicts with Codex work appear, stop and report before rewriting shared files.
