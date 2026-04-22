# ROADMAP: OpenGEM UX Integration (OPENGEM-UX-001)

## Purpose
Integrate OpenGEM as the primary desktop environment GUI in CiukiOS,
providing native DOS-compatible windowing and an app launcher for real
DOS workflows, en route to running real DOS DOOM binaries from
CiukiOS.

## Strategic Alignment
- **North Star:** Run real DOS DOOM binaries from CiukiOS.
- **Phase 4 (Desktop):** Current baseline done; next: windowing +
  app-launch UX for DOS workflows.
- **Phase 3 (FreeDOS):** OpenGEM already in symbiotic runtime pipeline.
- **Rationale:** OpenGEM is a real DOS GUI, standards-compatible, ready
  to integrate, and exercises the same `.BAT` / `.EXE MZ` / `.COM` paths
  DOOM will rely on.

## Phase Index
| # | ID | Title | Status | Gate |
|---|----|-------|--------|------|
| 1 | OPENGEM-001 | Runtime Validation & Launcher Integration | DONE | `make test-opengem-smoke` |
| 2 | OPENGEM-002-BAT | BAT Interpretation Hardening | DONE | `make test-bat-interp` |
| 3 | OPENGEM-003 | Desktop Scene Integration (windowing) | DONE | `make test-opengem-launch` |
| 4 | OPENGEM-004 | App Discovery and File Catalog | DONE | `make test-opengem-file-browser` |
| 5 | OPENGEM-005 | Input and Mouse Hardening in OpenGEM | DONE | `make test-opengem-input` |
| 6 | OPENGEM-006 | DOOM Path Readiness | DONE | `scripts/test_doom_via_opengem.sh` |

---

## Phase 1 — Runtime Validation & Launcher Integration (OPENGEM-001) · DONE
**Goal:** Ensure the OpenGEM runtime is on the FAT image, discoverable
from CiukiOS, and launchable from three surfaces (shell command, dock
item, desktop shortcut) via a single, testable code path.

### Tasks
1. Verify OpenGEM entry points in `third_party/freedos/runtime/OPENGEM/`
   (`GEM.BAT`, `GEMAPPS/GEMSYS/DESKTOP.APP`, companion apps).
2. Copy the OpenGEM tree into the FAT image via `run_ciukios.sh`
   (`::FREEDOS/OPENGEM/`).
3. Implement `shell_run_opengem_interactive(boot_info, handoff)` as the
   single launch entry; preserve the 5-candidate preflight probe.
4. Wire three entry surfaces to this helper:
   - `opengem` shell command
   - `OPENGEM` dock item (launcher items 6 → 7)
   - `ALT+O` desktop shortcut (precedes the `ALT+G+Q` chord)
5. Emit serial boot/launch/exit/fallback markers.
6. Add a host-side smoke gate that validates runtime payload, stage2
   wiring, launcher list, image pipeline, and (when available) boot-log
   markers.

### Deliverables
- `stage2/src/shell.c` · `shell_run_opengem_interactive()`
- `stage2/src/ui.c` · 7th launcher item `OPENGEM`
- `scripts/test_opengem_smoke.sh`
- `docs/opengem-runtime-structure.md`
- `make test-opengem-smoke`

### Serial marker vocabulary (frozen)
```
OpenGEM: boot sequence starting
OpenGEM: launcher window initialized
OpenGEM: exit detected, returning to shell
OpenGEM: runtime not found in FAT, fallback to shell
```

### Gate
- `make test-opengem-smoke` → **PASS** (13/13).
- `make test-opengem` → **PASS** (shell help contract).
- `make test-gui-desktop` → **PASS** (desktop discoverability contract).
- `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh` → **PASS**.

---

## Phase 2 — BAT Interpretation Hardening (OPENGEM-002-BAT) · DONE
**Goal:** Promote the existing in-tree BAT interpreter
(`shell_run_batch_file` in `stage2/src/shell.c`) from
"good enough for `AUTOEXEC.BAT` / our own demos" to a contract surface
that can actually run `GEM.BAT`, `SETUP.BAT`, and real FreeDOS batch
scripts reliably, with deterministic serial markers and a dedicated
test gate.

### Current State (2026-04-19)
Already present in the interpreter:
- CRLF-tolerant line splitter, label table (`:LABEL`),
  `SHELL_BATCH_MAX_LINES`, `SHELL_BATCH_MAX_LABELS`,
  `SHELL_BATCH_MAX_STEPS`, `SHELL_BATCH_MAX_DEPTH`.
- `REM` / empty-line skipping.
- `%VAR%` expansion via `shell_env_expand_line`.
- `GOTO <label>`.
- `IF ERRORLEVEL N GOTO <label>`.
- `SET NAME=VALUE`.
- `ECHO <text>` (sets ERRORLEVEL 0).
- Nested calls through `shell_execute_line` (indirect recursion into
  other `.BAT` files via `run`).
- `AUTOEXEC.BAT` auto-run on startup; `CONFIG.SYS` processed for
  `SHELL=` and `SET`.

### Gap Analysis — what real DOS `.BAT` (GEM.BAT in particular) needs
1. **`@ECHO OFF` / echo state.** GEM.BAT opens with `@echo off`; we do
   not toggle line echo and the leading `@` is currently not stripped.
2. **`CALL <script.bat>`** returning to caller context; nested batch
   currently works only through ad-hoc command resolution.
3. **`CD` / `CHDIR` inside batch.** GEM.BAT does `CD \GEMAPPS\GEMSYS`
   as its first operation. Must route to the shell's existing CWD
   mutation, not be silently no-op'd.
4. **`PAUSE`** (used by SETUP.BAT) — wait for keypress.
5. **Positional arguments `%0`..`%9`.** GEM.BAT forwards
   `%1 %2 %3` to GEMVDI. Need argv capture at batch invocation time
   and substitution during expansion.
6. **`SHIFT`** — shifts `%1`..`%9`. Lower priority but cheap.
7. **`IF "%X%"=="value" ...`** string equality.
8. **`IF EXIST <path> ...`** — used by GEMSYS startup guards.
9. **`GOTO :EOF`** convention — clean end of the current batch.
10. **`%ERRORLEVEL%`** variable form (in addition to `IF ERRORLEVEL`).
11. **`::` comments** (alternate REM form).
12. **Quoted tokens** in `IF ""==""` comparisons.

### Tasks
1. **`@ECHO` state machine**
   - Per-frame `g_batch_echo` (default ON).
   - Strip leading `@` before interpretation; `@` forces echo off for
     that line only.
   - Handle `ECHO OFF`, `ECHO ON`, `ECHO.`, `ECHO <text>` as before.
   - When echo is ON and the line is not `ECHO`/`REM`/`:LABEL`, print
     the expanded line before executing it.
2. **Arg passing**
   - Extend `shell_run_batch_file()` to accept
     `(int argc, const char *const *argv)`; keep a zero-arg wrapper.
   - Positional resolver in `shell_env_expand_line`:
     `%0` → batch path, `%1`..`%9` → captured argv, missing → empty.
   - Implement `SHIFT`.
3. **Flow control**
   - `CALL <script[.BAT]> [args]`: push `(pc, argv)`, run callee,
     restore.
   - `GOTO :EOF` → set `pc = line_count`.
   - `::` comment → treat as blank.
4. **CD/CHDIR passthrough**
   - Ensure `CD`, `CHDIR`, and bare `\PATH` (implicit `CD \PATH`)
     route through `shell_cmd_cd` with the canonical path resolver.
5. **PAUSE**
   - Blocking read of a single keystroke with the standard
     `"Press any key to continue . . ."` line.
6. **IF expansions**
   - `IF [NOT] EXIST <path>` using `fat_find_file`.
   - `IF [NOT] "<a>"=="<b>"` string equality after env expansion.
   - Keep existing `IF ERRORLEVEL N` path untouched.
7. **Serial markers**
   ```
   [ bat ] enter <path> argc=<n>
   [ bat ] line <pc>: <expanded-line>        (only when echo on)
   [ bat ] goto <label> -> line <n>
   [ bat ] call <path>
   [ bat ] return from <path> errorlevel=<n>
   [ bat ] exit <path> errorlevel=<n>
   [ bat ] aborted <reason>
   ```
8. **Integration with OpenGEM**
   - Verify `GEM.BAT` runs to its `GEMVDI %1 %2 %3` step (may still
     fail at GEMVDI execution — that is Phase 3/5 territory; the Phase
     2 gate is "the interpreter does not stop early").
   - Emit marker `[ bat ] gem.bat reached gemvdi invocation` when a
     batch whose basename matches `GEM.BAT` reaches the last line
     without aborting.

### Deliverables
- `stage2/src/shell.c` BAT interpreter upgrade (all tasks above).
- `scripts/test_bat_interp.sh` — host-side smoke gate with fixture
  files under `tests/bat/`:
  - `minimal.bat` (echo off + basic cmds + exit errorlevel 0)
  - `args.bat` (verifies `%1 %2 %3` + `SHIFT`)
  - `flow.bat` (GOTO + IF EXIST + IF ""== + CALL + GOTO :EOF)
  - `pause-skip.bat` (uses `PAUSE` behind a `GOTO` so it never runs)
  The gate validates:
  - All fixture files parse.
  - Static stage2 source contains the new keywords
    (`@echo`, `CALL `, `PAUSE`, `IF EXIST`, `GOTO :EOF`, `%0..%9`).
  - Makefile exposes `test-bat-interp`.
  - If a boot log is captured, serial `[ bat ]` markers for
    `minimal.bat` are present.
- `Makefile` target `test-bat-interp`.
- `docs/bat-interpreter.md` — contract: supported keywords,
  unsupported, limits (`MAX_LINES` / `MAX_STEPS` / `MAX_DEPTH`),
  marker catalogue, known divergences from real `COMMAND.COM`.

### Validation
1. `make test-bat-interp` → PASS.
2. `make test-opengem-smoke` still PASS.
3. `make test-opengem` still PASS.
4. `make test-gui-desktop` still PASS.
5. Full macOS pipeline still PASS.
6. AUTOEXEC.BAT startup still produces the same user-visible output
   on the text console.

### Risks
- **Echo semantics drift.** Real `COMMAND.COM` prints the expanded
  line with trailing CRLF *before* executing; our ring buffer wraps
  differently. Mitigation: keep the echo behind a serial-only marker
  until visual regression budget allows a UX polish.
- **`SHIFT` corner cases.** `%1` becoming empty after shift must not
  throw; resolver returns `""`.
- **Quoting.** DOS quoting rules are weird. Scope: handle `"…"` as a
  single token *only* inside `IF ""==""` — do not try to be a real
  shell parser.

### Gate
`make test-bat-interp` **PASS** + zero regressions across the existing
opengem/desktop/stage2 gates.

---

## Phase 3 — Desktop Scene Integration (OPENGEM-003) · DONE
**Goal:** Promote the current text-mode dock entry from
"label + launcher-dispatch" to a real OpenGEM-aware window management
story: the desktop knows it just handed control to a GEM session, can
show a "OpenGEM running…" overlay while the child session owns the
screen, and restores the dock on return.

### Status
- **Done** (Phase 1): `OPENGEM` dock item, `ALT+O` shortcut, helper
  dispatch, serial markers, graceful text-mode fallback when payload
  missing.
- **Open**: visible icon glyph, explicit desktop-state save/restore
  across the launch, "running…" overlay, blur/disable other dock
  items while OpenGEM owns the screen.

### Tasks
1. Replace the textual `OPENGEM` label with a 24×24 glyph (ASCII-art
   GEM-style facsimile for text mode; a 4-bit palette tile when a
   planar mode is active).
2. Desktop state save before launch: dock selection index, layout
   zones (if any active), status line text; serialize into a compact
   struct on the stage2 stack frame.
3. While `shell_run_opengem_interactive()` is inside `shell_run()`,
   render a static "OpenGEM running — press ALT+G+Q inside OpenGEM to
   exit" line to the serial console at entry so telemetry can
   correlate boot-log to UI state.
4. On return, restore the dock selection index and status line.
5. Clean fallback: when preflight fails, pop a modal-style line with
   `OPENGEM: n/a — payload not installed` and return focus to the
   launcher on the previous selection.

### Deliverables
- `stage2/src/ui.c` glyph and optional icon renderer.
- `shell.c`: desktop-state struct + save/restore around the helper
  call.
- `scripts/test_opengem_launch.sh` + `make test-opengem-launch` to
  assert launcher state preservation with static + boot-log probes.
- `docs/opengem-runtime-structure.md` updated with the state-save
  contract.

### Markers
```
[ ui ] opengem dock state saved: sel=<n>
[ ui ] opengem overlay active
[ ui ] opengem overlay dismissed, state restored
```

### Gate
`make test-opengem-launch` PASS + `make test-gui-desktop` still PASS +
`make test-opengem-smoke` still PASS.

### Risks
- **State drift** if the launcher gains items while OpenGEM is
  running — bound by the fact that stage2 is single-threaded and the
  dock list is static at compile time; the restored selection index
  is still valid.
- **Overlay flicker** on slow text consoles — mitigation: only draw
  the overlay line once on entry.

---

## Phase 4 — App Discovery and File Catalog (OPENGEM-004) · DONE
**Goal:** Populate OpenGEM with discoverable DOS applications sourced
from the existing COM catalog and from the FAT image, so the GUI can
launch real `.COM` / `.EXE` / `.BAT` targets rather than its four
bundled demo apps.

### Tasks
1. Scan FAT roots `/`, `/FREEDOS`, `/FREEDOS/OPENGEM`,
   `/EFI/CiukiOS` for `*.COM`, `*.EXE`, `*.BAT`.
2. Export the existing COM catalog (`com/*` build outputs +
   `build/*.COM` shipped in the image) as a second discovery lane.
3. Join both lanes into a de-duplicated `app_catalog_t` ABI surface:
   `{ char name[13]; char path[64]; u8 kind; u8 reserved[3]; }`
   (append-only; new fields only at the tail).
4. Expose the catalog through the stable services ABI
   (`ciuki_services_t.app_catalog`) so a future GEMVDI host-app can
   iterate it.
5. Add a `catalog` shell command that prints the joined list
   (grepable for gates).
6. When a BAT invokes a name with no full path, extend the existing
   PATH resolver to also probe catalog entries with kind=BAT.

### Deliverables
- `stage2/include/app_catalog.h` ABI.
- `stage2/src/app_catalog.c` discovery + dedupe.
- Wiring in `shell.c` (`catalog` command + PATH integration).
- `scripts/test_opengem_file_browser.sh` asserting:
  - Catalog structurally present.
  - `catalog` command lists at least the shipped COMs.
  - OpenGEM payload entries present under `/FREEDOS/OPENGEM`.
- `make test-opengem-file-browser`.

### Markers
```
[ catalog ] scan begin root=<path>
[ catalog ] scan entry <name> kind=<com|exe|bat> path=<path>
[ catalog ] scan done entries=<n> roots=<m>
```

### Gate
`make test-opengem-file-browser` PASS; no regressions.

### Risks
- **FAT scan cost** on slow media — bound by
  `APP_CATALOG_MAX_ENTRIES = 256`, early-out per directory.
- **Name collisions** between catalog and FAT — deterministic
  tie-break: FAT wins (users can override bundled demos by dropping a
  COM on the image).

---

## Phase 5 — Input and Mouse Hardening in OpenGEM (OPENGEM-005) · DONE
**Goal:** Ensure INT 33h mouse and INT 16h keyboard already delivered
by SR-MOUSE-001 behave correctly once OpenGEM/GEMVDI owns the screen;
OpenGEM is a historically mouse-heavy GUI.

### Tasks
1. Validate INT 33h mouse state is preserved across the
   desktop → OpenGEM → shell transition (no reset on helper
   entry/exit).
2. Ensure the software cursor blitter
   (`svc.mouse_draw_cursor_mode13`) is quiescent while GEMVDI has
   control; GEMVDI renders its own pointer.
3. Add a guarded bridge `int33_hooks_t` so OpenGEM can install its
   own INT 33h event callback without breaking the fallback path.
4. Instrument keyboard routing so ALT+G+Q inside OpenGEM still
   reaches the shell exit code path (document the chord as the
   escape hatch if OpenGEM locks up).
5. Input-latency telemetry: capture wall-clock between `IRQ12` and
   `[ mouse ] delivered` markers; target ≤ 10 ms at boot.

### Deliverables
- `stage2/include/mouse.h` new `int33_hooks_t` append-only surface.
- Light code in `shell.c` to quiesce the CiukiOS cursor during
  OpenGEM sessions.
- `scripts/test_opengem_input.sh` static + boot-log probe.
- `make test-opengem-input`.

### Markers
```
[ mouse ] opengem session: cursor disabled
[ mouse ] opengem session: cursor restored
[ mouse ] opengem hook installed
[ kbd ] opengem escape chord: alt+g+q detected
```

### Gate
`make test-opengem-input` PASS + `make test-mouse-smoke` still PASS +
`make test-stage2` still PASS (when the host allows it).

### Risks
- **Hook ABI drift.** Mitigation: append-only field order, version
  byte.
- **Lost clicks** during the window where CiukiOS cursor is disabled
  but GEMVDI hasn't yet installed its own — acceptable: GEMVDI boots
  fast; mark as a known gap if observed.

---

## Phase 6 — DOOM Path Readiness (OPENGEM-006) · DONE
**Goal:** End-to-end demonstration that a user-supplied, shareware
DOOM binary can be placed on the FAT image, discovered by the Phase 4
catalog, and launched from OpenGEM (or from the shell via the same
paths OpenGEM exercises).

### Tasks
1. Stage DOOM under `/GAMES/DOOM/` in the FAT image via an opt-in
   flag on `run_ciukios.sh` (binary remains user-supplied, per
   licensing policy).
2. Verify catalog discovery picks up `DOOM.EXE` + `DOOM1.WAD`.
3. Extend `scripts/test_doom_via_opengem.sh` to chain:
   preflight → catalog probe → DOOM launch marker →
   `menu_reached` classification (already a stage of the existing
   DOOM-boot harness).
4. Produce a boot-to-DOOM flow diagram rooted at OpenGEM (replacing
   the current shell-only path).
5. Document compatibility gaps surfaced by steps 1–3 and file them
   against the appropriate follow-up phase (BAT, input, video mode
   13h, DOS/4GW extender).

### Deliverables
- `scripts/test_doom_via_opengem.sh` gated behind
  `CIUKIOS_DOOM_FIXTURES_DIR` (runs only when a user has provided
  shareware fixtures locally).
- `docs/boot-to-doom-via-opengem.md` flow diagram + gap list.
- `Makefile` target `test-doom-via-opengem` (skip when fixtures
  absent).

### Markers
```
[ doom ] catalog discovered DOOM.EXE at <path>
[ doom ] catalog discovered DOOM1.WAD at <path>
[ doom ] opengem launch DOOM.EXE
[ doom ] stage reached: menu
```

### Gate
`scripts/test_doom_via_opengem.sh` PASS when
`CIUKIOS_DOOM_FIXTURES_DIR` is set; SKIP otherwise. No regressions
elsewhere.

### Risks
- **Shareware licensing:** binary is user-supplied; CI does not
  redistribute.
- **DOS/4GW dependency:** real DOOM is a DOS/4GW client. The DOS
  extender bootstrap milestones (M6 DPMI smokes) are the long-tail
  dependency; Phase 6 will *use* whatever extender state exists and
  report gaps rather than block on it.

---

## Success Criteria (roll-up)

### Functional
1. OpenGEM boots and displays launcher.
2. Shell `opengem`, dock `OPENGEM`, and `ALT+O` all launch OpenGEM.
3. BAT interpreter executes GEM.BAT / SETUP.BAT / AUTOEXEC.BAT
   without early abort.
4. Launcher discovers and executes DOS `.COM`/`.EXE`/`.BAT` from the
   catalog + FAT.
5. Mouse and keyboard remain functional across the
   desktop ↔ OpenGEM ↔ shell transition.
6. Optional DOOM path available when fixtures are user-provided.

### Compatibility
1. No regressions to shell/video/desktop/mouse tests.
2. INT 33h mouse API stable (append-only extensions only).
3. FAT filesystem access stable.
4. FreeDOS symbiotic integration unchanged.
5. BAT interpreter behaves as a strict subset of `COMMAND.COM` —
   documented divergences only.

### Documentation
1. `docs/opengem-runtime-structure.md` stable contract.
2. `docs/bat-interpreter.md` keyword + marker catalogue.
3. `docs/boot-to-doom-via-opengem.md` flow diagram.
4. One handoff per phase under `docs/handoffs/`.

### Testing
1. `make test-opengem-smoke` PASS.
2. `make test-bat-interp` PASS.
3. `make test-opengem-launch` PASS.
4. `make test-opengem-file-browser` PASS.
5. `make test-opengem-input` PASS.
6. `make test-doom-via-opengem` PASS when fixtures present, SKIP
   otherwise.
7. All existing gates remain PASS (no regressions).

---

## Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| OpenGEM payload not installed | Graceful fallback with `OpenGEM: runtime not found…` marker (Phase 1 contract) |
| BAT keyword coverage gaps | Phase 2 static gate + fixture BATs + documented divergences |
| Mouse/keyboard regressions | Phase 5 hook ABI + regression gates kept green |
| FAT catalog scan cost | Per-root entry cap + early-out; Phase 4 |
| DOOM binary compatibility | Staged harness (Phase 6) reports gaps instead of blocking |
| Desktop state corruption | Explicit save/restore struct around the helper (Phase 3) |
| Version churn | No version bumps inside phases; only on user-requested cut |

---

## Execution Contract Across All Phases
1. One task per phase, one branch per task, one handoff per task.
2. Never commit to `main`; merge only on explicit "fai il merge".
3. No version bump inside a phase; baseline `CiukiOS Alpha v0.8.7`
   is owned by the user.
4. ABI changes are append-only (services table, mouse hooks,
   catalog).
5. Every new behavior ships a gate; every gate has a marker
   vocabulary.
6. Pre-existing gates stay green; if a gate drifts, fix it in the
   same commit that changes the contract it protects (as done at the
   end of Phase 1 for `test-opengem` and `test-gui-desktop`).

---

## Timeline Estimate (indicative, per-session effort)
- Phase 1 (DONE): 1 session.
- Phase 2 (BAT): 2 sessions (interpreter + gate + docs).
- Phase 3 (desktop integration): 1–2 sessions.
- Phase 4 (catalog): 2 sessions.
- Phase 5 (input): 1 session.
- Phase 6 (DOOM): 1–2 sessions (fixture-dependent).

**Total remaining after Phase 1: ~7–9 focused sessions.**

---

## Related Documents
- `docs/freedos-integration-policy.md` — OpenGEM licensing and
  runtime policy.
- `docs/freedos-symbiotic-architecture.md` — integration pattern.
- `docs/opengem-runtime-structure.md` — runtime layout (Phase 1
  contract).
- `stage2/include/mouse.h` — mouse driver (INT 33h).
- `Roadmap.md` — main project roadmap.
- `CLAUDE.md` — collaboration directives.

---

## Version
- Created: 2026-04-19
- Last edit: 2026-04-19 (added Phase 2 — BAT Interpretation
  Hardening, renumbered downstream phases, expanded every phase to
  full contract level)
- Status: Phase 1 DONE, Phase 2+ PLANNED.
- Next Review: after the first commit of Phase 2
  (OPENGEM-002-BAT).
