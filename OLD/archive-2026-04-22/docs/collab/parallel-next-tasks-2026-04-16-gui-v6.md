# Parallel Next Tasks (2026-04-16) - GUI Alignment v6

Context:
- Current desktop GUI is functional but still has visible alignment defects.
- Screenshot issues observed: dock text overflow, inconsistent panel spacing, title/content padding mismatch.
- This cycle is visual hardening only (no DOS core behavior changes).

Global Constraints (all tasks):
1. Allowed scope only: `stage2/src/ui.c`, `stage2/include/ui.h`, and only minimal helper exposure in `stage2/src/video.c` + `stage2/include/video.h` if strictly required for text metrics/clipping.
2. Do NOT modify loader/handoff ABI/INT21/FAT/shell parser/build scripts.
3. Preserve keyboard desktop navigation (`TAB`, `J/K`, arrows, `ENTER`).
4. Keep deterministic grid-aligned rendering and 800x600 + 1280x800 compatibility.
5. Add one handoff for each major task batch.

## GitHub Copilot Tasks (GUI)

### Task G6 - Replace desktop exit key with chord `ALT+G+Q`
Suggested branch:
- `feature/copilot-gui-exit-chord-v6`

Goal:
- Replace `ESC` exit behavior with a safer chord: hold `ALT`, then `G`, then `Q`.

Scope (owned files):
- `stage2/src/keyboard.c`
- `stage2/include/keyboard.h`
- `stage2/src/shell.c`
- (optional) `stage2/src/ui.c` only for updated footer hint text

Deliverables:
1. Add ALT state tracking in keyboard decode path (press/release).
2. Add chord detector in desktop session loop.
3. Remove `ESC` as primary exit path from desktop mode.
4. Update user hints in help/footer text.
5. Add serial marker: `[ ui ] desktop exit chord alt+g+q active`.

Acceptance:
1. Desktop exits only via `ALT+G+Q` (or documented fallback if explicitly retained).
2. No regression in command loop after exiting desktop.
3. `make test-stage2` and `make test-int21` PASS.

---

### Task G7 - Dock text clipping + truncation safety
Suggested branch:
- `feature/copilot-gui-dock-clipping-v6`

Goal:
- Prevent any dock label from crossing the dock visual bounds.

Scope (owned files):
- `stage2/src/ui.c`
- `stage2/include/ui.h`

Deliverables:
1. Introduce text-in-rect helper with hard clipping/truncation.
2. Ensure long labels (e.g. `RUN INIT.COM`) stay inside dock rect.
3. Apply ellipsis or compact aliases on narrow layouts.
4. Keep selected-row highlight fully containing rendered label.

Acceptance:
1. Zero dock text overflow in 800x600 and 1280x800.
2. No overlap between dock labels and workspace windows.
3. `make test-gui-desktop` PASS.

---

### Task G8 - Window chrome and inner padding normalization
Suggested branch:
- `feature/copilot-gui-window-padding-v6`

Goal:
- Normalize title/content offsets so all windows look intentional and aligned.

Scope (owned files):
- `stage2/src/ui.c`
- `stage2/include/ui.h`

Deliverables:
1. Centralize spacing tokens (`panel_padding_x/y`, `titlebar_h`, `content_inset`).
2. Ensure titles never sit on border lines.
3. Ensure content rows begin at consistent baseline across System/Shell/Info windows.
4. Keep all panel geometry snapped to grid.

Acceptance:
1. Window title + content alignment is visually consistent.
2. No text touching borders.
3. `make test-stage2` PASS.

---

### Task G9 - Status/footer responsive text policy
Suggested branch:
- `feature/copilot-gui-footer-responsive-v6`

Goal:
- Make footer text always readable and inside bounds at different widths.

Scope (owned files):
- `stage2/src/ui.c`

Deliverables:
1. Add width-aware footer message variants (long + compact).
2. Ensure footer text never overlaps border/panel edges.
3. Keep functional hints up to date with `ALT+G+Q` exit behavior.

Acceptance:
1. Footer remains legible at 800x600.
2. No clipping or overdraw on status bar borders.
3. `make test-gui-desktop` PASS.

---

### Task G10 - GUI visual regression gate v2
Suggested branch:
- `feature/copilot-gui-regression-gate-v2`

Goal:
- Strengthen GUI regression checks for alignment/clipping markers.

Scope (owned files):
- `scripts/test_gui_desktop.sh`
- `docs/handoffs/` (new handoff)

Deliverables:
1. Validate presence of new markers:
   - `[ ui ] desktop exit chord alt+g+q active`
   - `[ ui ] alignment surgical v6 active` (or newer equivalent marker)
2. Add checks/warnings for known visual risk zones (dock overflow, footer clipping).
3. Keep non-interactive behavior tolerant (warnings where interactive markers are expected).

Acceptance:
1. `make test-gui-desktop` PASS with clearer diagnostics.
2. Existing test suite behavior unchanged outside GUI checks.

## Merge Gate (required)
1. `make test-stage2`
2. `make test-int21`
3. `make test-gui-desktop`

## Handoff Rule
- Create: `docs/handoffs/2026-04-16-copilot-gui-v6-<topic>.md`
- Include:
  1. Problem observed
  2. What changed and why
  3. Tests executed
  4. Residual risks
