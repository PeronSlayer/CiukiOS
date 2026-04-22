# Parallel Next Tasks (2026-04-16) - GUI + DOS Roadmap

Context:
- Active agents: Codex + GitHub Copilot (Claude Haiku 4.5)
- Priority split for this cycle:
  1. Start visible GUI evolution (safe, incremental)
  2. Continue DOS-compat roadmap on INT21 core behavior

## GitHub Copilot Tasks (GUI)

### Task G1 - UI Primitives Module (Stage2)
Suggested branch:
- `feature/copilot-gui-ui-primitives`

Goal:
- Create a reusable Stage2 GUI utility layer so rendering logic is no longer scattered in `stage2.c`.

Scope (owned files):
- `stage2/include/ui.h` (new)
- `stage2/src/ui.c` (new)
- `stage2/src/stage2.c` (wire-in only)
- `Makefile` (only if needed for new source object discovery)

Deliverables:
1. Add primitives:
   - top bar draw
   - centered label draw
   - progress bar draw
   - panel/frame draw helper
2. Refactor current splash footer/title-bar drawing code in `stage2.c` to call UI helpers.
3. Preserve current visual behavior (no regressions in boot logs / shell startup).

Acceptance:
1. `make test-stage2` PASS
2. `make test-fallback` PASS
3. No changes to loader ABI/handoff structs
4. New module documented in handoff

### Task G2 - Boot HUD (Status Overlay)
Suggested branch:
- `feature/copilot-gui-boot-hud`

Goal:
- Add a minimal, readable HUD in graphical mode showing runtime status in real-time during boot.

Scope (owned files):
- `stage2/src/stage2.c`
- `stage2/src/ui.c` / `stage2/include/ui.h` (reuse from G1)
- `scripts/test_stage2_boot.sh` (only if adding a new stable marker)

Deliverables:
1. Display a compact HUD section with:
   - `CiukiOS` label
   - stage2 version
   - current mode (`gfx`/`ascii`)
   - loading progress percent
2. Add one deterministic serial marker for HUD activation:
   - `[ ui ] boot hud active`

Acceptance:
1. `make test-stage2` PASS (including HUD marker if asserted)
2. No ticker flood regression
3. No blocking loops; boot proceeds to shell normally

## Codex Tasks (Roadmap Continuation)

### Task C1 - INT21 Keyboard Status/Flush (AH=0Bh, AH=0Ch)
Suggested branch:
- `feature/codex-int21-kbd-status-flush`

Goal:
- Extend INT21 compatibility with deterministic keyboard polling/flush semantics.

Scope (owned files):
- `stage2/src/shell.c`
- `docs/int21-priority-a.md`
- `scripts/test_int21_priority_a.sh`

Deliverables:
1. Implement `AH=0Bh` (keyboard status)
2. Implement `AH=0Ch` (flush input buffer + dispatch input function subset deterministically)
3. Extend `stage2_shell_selftest_int21_baseline()` accordingly
4. Update INT21 matrix/status docs

Acceptance:
1. `make test-int21` PASS
2. `make check-int21-matrix` PASS
3. Unsupported subfunctions remain deterministic (`CF/AX` rules)

### Task C2 - INT21 Handle API Deterministic Baseline (3Ch..42h)
Suggested branch:
- `feature/codex-int21-handle-baseline`

Goal:
- Prepare M3/M5 bridge by adding deterministic, testable behavior for handle-based file API surface.

Scope (owned files):
- `stage2/src/shell.c`
- `docs/int21-priority-a.md`
- `scripts/check_int21_matrix.sh` (if matrix categories expand)

Deliverables:
1. Add deterministic handling for:
   - `AH=3Ch` create
   - `AH=3Dh` open
   - `AH=3Eh` close
   - `AH=3Fh` read
   - `AH=40h` write
   - `AH=41h` delete
   - `AH=42h` lseek
2. In this phase, allow a mixed strategy:
   - minimally real behavior where safe
   - deterministic stubs where backend is not ready
3. Ensure explicit DOS-like error mapping (`CF=1`, AX code) for invalid/unsupported conditions.

Acceptance:
1. `make test-int21` PASS
2. `make check-int21-matrix` PASS
3. Existing boot/fallback/FAT tests remain green

## Merge Gate (All Branches)
1. `make check-int21-matrix`
2. `make test-stage2`
3. `make test-fallback`
4. `make test-fat-compat`
5. `make test-int21`
6. `make test-freedos-pipeline`

## Handoff Rule
- Every major multi-file task must add:
  - `docs/handoffs/YYYY-MM-DD-<topic>.md`
