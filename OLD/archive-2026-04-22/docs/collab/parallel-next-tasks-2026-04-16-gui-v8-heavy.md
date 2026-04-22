# Parallel Next Tasks (2026-04-16) - GUI Heavy Cycle v8

Context:
- Desktop is now interactive and mostly stable, but still visually minimal.
- Goal of this cycle: significant feature growth + quality hardening (not cosmetic-only).
- Keep retro desktop direction, deterministic rendering, zero regressions on DOS/runtime core.

Global constraints:
1. Allowed scope first: `stage2/src/ui.c`, `stage2/include/ui.h`, `stage2/src/shell.c`, `stage2/src/keyboard.c`, `stage2/include/keyboard.h`, `scripts/test_gui_desktop.sh`.
2. Optional scope only if strictly needed: `stage2/src/video.c`, `stage2/include/video.h`.
3. Do NOT touch: loader/handoff ABI, FAT core, INT21 core semantics, FreeDOS pipeline.
4. Every task must keep 800x600 and 1280x800 clean.
5. No magic numbers: use named design/layout tokens.

## Task G11 - Desktop Launcher Actions (real command dispatch)
Suggested branch:
- `feature/copilot-gui-launcher-actions-v8`

Goal:
- Launcher items must execute real shell actions, not just serial markers.

Deliverables:
1. Map launcher entries to actual commands (`DIR`, `MEM`, `CLS`, `VER`, `ASCII`, `RUN INIT.COM`).
2. Keep desktop scene active after command completion (unless explicit exit command used).
3. Add visual feedback for action status (`running`, `ok`, `error`) in info/status panel.
4. Serial marker: `[ ui ] launcher action dispatch active`.

Acceptance:
1. ENTER on launcher item executes corresponding command.
2. No freeze/reentry bug after command execution.
3. `make test-stage2` and `make test-gui-desktop` PASS.

---

## Task G12 - Desktop Command Console Panel
Suggested branch:
- `feature/copilot-gui-console-panel-v8`

Goal:
- Add an internal mini-console area inside desktop mode to show recent action output lines.

Deliverables:
1. Add ring buffer (last N lines) rendered in `Shell` window.
2. Route launcher command logs into this panel.
3. Add clear-console shortcut in desktop mode (`Ctrl+L` or alternative documented key).
4. Serial marker: `[ ui ] desktop console panel active`.

Acceptance:
1. Running launcher actions writes visible logs in Shell panel.
2. Buffer scrolls deterministically and never overflows window bounds.
3. `make test-gui-desktop` PASS.

---

## Task G13 - Window Layout Manager v3 (adaptive + ratio tokens)
Suggested branch:
- `feature/copilot-gui-layout-manager-v3`

Goal:
- Replace fixed split assumptions with tokenized ratio layout adaptable to small and wide screens.

Deliverables:
1. Define ratio tokens for left/right columns and top/bottom split.
2. Introduce minimum window sizes and fallback layout when constraints fail.
3. Prevent tiny unreadable window regions at 800x600.
4. Serial marker: `[ ui ] desktop layout manager v3 active`.

Acceptance:
1. No overlap/clipping in 800x600 and 1280x800.
2. Title/content regions always non-zero and readable.
3. `make test-gui-desktop` PASS.

---

## Task G14 - Focus and Selection UX polish (keyboard-first)
Suggested branch:
- `feature/copilot-gui-focus-ux-v8`

Goal:
- Make focus state obvious and consistent for window focus + launcher selection.

Deliverables:
1. Distinct visual styles for focused window vs unfocused windows.
2. Distinct selection style for active launcher item and hover-equivalent keyboard focus.
3. Add a small focus legend in status bar (e.g. `Focus: System|Shell|Info`).
4. Serial marker: `[ ui ] desktop focus ux v8 active`.

Acceptance:
1. TAB cycle always visible and predictable.
2. Launcher focus never desyncs from rendered highlight.
3. `make test-gui-desktop` PASS.

---

## Task G15 - Desktop Session State Machine Hardening
Suggested branch:
- `feature/copilot-gui-session-sm-v8`

Goal:
- Stabilize desktop enter/run/exit transitions as an explicit state machine.

Deliverables:
1. State enum for desktop session lifecycle (`ENTERING`, `ACTIVE`, `RUNNING_ACTION`, `EXITING`).
2. Guard against re-entrant action dispatch while action is already running.
3. Ensure `ALT+G+Q` works from any ACTIVE sub-state without deadlock.
4. Serial marker: `[ ui ] desktop session state-machine v8 active`.

Acceptance:
1. No stuck states after repeated command dispatches.
2. Exit chord still reliable on QEMU.
3. `make test-stage2`, `make test-gui-desktop` PASS.

---

## Task G16 - GUI Regression Gate v3 (stronger assertions)
Suggested branch:
- `feature/copilot-gui-regression-gate-v3`

Goal:
- Expand GUI automated gate to validate new v8 markers and state transitions.

Deliverables:
1. Extend `scripts/test_gui_desktop.sh` with v8 marker set.
2. Validate marker ordering constraints for key state transitions where possible.
3. Keep tolerant behavior for non-interactive logs (warn vs fail where appropriate).
4. Add section in test output: `GUI v8 capability summary`.

Acceptance:
1. Gate catches missing v8 integrations.
2. No false-fail on non-interactive CI logs.
3. `make test-gui-desktop` PASS.

## Merge gate (required)
1. `make check-int21-matrix`
2. `make test-stage2`
3. `make test-int21`
4. `make test-gui-desktop`

## Handoff rule
Create handoff:
- `docs/handoffs/2026-04-16-copilot-gui-v8-<topic>.md`

Must include:
1. Problem + root cause
2. Exact changes
3. Tests and results
4. Residual risks
