# 2026-04-19 — OPENGEM-003: Desktop Scene Integration

## Context and goal
Phase 3 of the OpenGEM UX roadmap
([docs/roadmap-opengem-ux.md](../roadmap-opengem-ux.md)). Promote the
OpenGEM launch path from "dispatch only" to a desktop-aware session
that snapshots and restores launcher state across the helper call,
emits overlay telemetry markers, renders a text-mode facsimile glyph
on the `OPENGEM` dock item, and keeps a graceful text-console
fallback when the payload is missing. No version bump (baseline
`CiukiOS Alpha v0.8.7`). No changes to `main`.

## Files touched
- `stage2/include/ui.h` — append-only launcher focus accessors:
  `ui_get_launcher_focus`, `ui_set_launcher_focus`,
  `ui_launcher_item_count` (comment block marks them as OPENGEM-003
  additions).
- `stage2/src/ui.c` — new `ui_launcher_display_for()` that maps the
  canonical `"OPENGEM"` action key to the visible `"[G] OPENGEM"`
  label; dock renderer now calls it. New public accessor
  implementations clamp out-of-range indices.
- `stage2/src/shell.c` · `shell_run_opengem_interactive()` —
  stack-allocated `desktop_snapshot` at entry, restored on every
  return path (preflight fail + normal return). Emits Phase 3 serial
  markers and prints `OpenGEM running - press ALT+G+Q ...` banner +
  `OPENGEM: n/a - payload not installed` modal fallback line.
- `scripts/test_opengem_launch.sh` — new host-side static smoke gate
  (24 assertions) + opt-in boot-log probe.
- `Makefile` — new target `test-opengem-launch`.
- `docs/opengem-runtime-structure.md` — documents the
  `desktop_snapshot` contract, the three Phase 3 markers, and the
  `ui_launcher_display_for()` glyph policy.
- `docs/roadmap-opengem-ux.md` — Phase 3 status flipped to DONE.
- `documentation.md` — new item 14 in Current Project State.
- `docs/handoffs/2026-04-19-opengem-003-desktop.md` — this file.
- `docs/collab/diario-di-bordo.md` — local diary entry (gitignored).

## Decisions made
- **Canonical action key vs. display label.** Kept `g_launcher_items[]`
  at `"OPENGEM"` (action key used by `desktop_dispatch_action` and
  grep'd by existing gates/help contracts). Applied the `[G]`
  facsimile glyph purely at render time through
  `ui_launcher_display_for()`. This avoids rippling into dispatch
  code and keeps `test-opengem` / `test-gui-desktop` green without
  any gate rewiring.
- **Stack-allocated snapshot.** `desktop_snapshot` lives on the
  helper's stack frame, captured once at entry and used only by the
  helper itself. No globals, no dynamic allocation, no ABI
  implications.
- **Focus-only restoration in Phase 3.** The snapshot carries
  `launcher_focus` + reserved `status0[64]` + `valid` byte. The
  status restore wire is deferred: the current desktop session
  already writes `"Running..."` → action-specific status on each
  dispatch, so forcibly restoring status on return would fight the
  dispatcher. The `status0[]` slot is reserved append-only so a
  future phase can opt in.
- **Modal line on text console.** The roadmap asks for a "modal-
  style line" on fallback. Implemented as a plain `video_write()`
  line rather than a real modal overlay: stage2 text mode has no
  window manager, and a modal would need its own input loop. The
  line is paired with the existing `OPENGEM: runtime not found ...`
  serial marker to give the smoke gate two independent assertions.
- **Overlay marker pair.** `[ ui ] opengem overlay active` fires on
  successful preflight (before `shell_run`); `[ ui ] opengem
  overlay dismissed, state restored` fires on every exit path so
  telemetry can always close the bracket — even on fallback, where
  the snapshot restore still runs.
- **No rename of `desktop_dispatch_action` matching.** Dispatch
  still matches `str_eq_nocase(action, "OPENGEM")` against the
  canonical item key, which `ui_get_launcher_item()` returns
  unchanged.

## Validation performed
- `make test-opengem-launch` → **PASS** (24 OK / 0 FAIL).
- `make test-bat-interp` → **PASS** (Phase 2 regression).
- `make test-opengem-smoke` → **PASS**.
- `make test-opengem` → **PASS** (help contract unchanged).
- `make test-gui-desktop` → **PASS** (launcher contract unchanged).
- `make test-mouse-smoke` → **PASS** (static fallback).
- `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh` → **PASS** (full
  stage2 / COMs / FAT image / OpenGEM payload build).

`make test-stage2` / `make test-fallback` remain blocked on macOS
per `/memories/repo/ciukios-build-notes.md`; not introduced by this
task.

## Risks and next step
- **Status-line restore deferred.** The `status0[64]` reserve is not
  yet populated. If a caller repaints the system status window in a
  way that needs post-OpenGEM restoration, it will not happen
  automatically. Documented in the snapshot block. Fix path:
  introduce `ui_get_window_status(0, buf, sz)` and wire it into the
  snapshot — append-only, future phase.
- **Glyph is text-mode only.** The roadmap calls for a 24×24
  palette tile in planar video mode. Current stage2 remains in
  text mode for the OpenGEM launch path; the `[G]` facsimile is the
  documented placeholder until the planar path lights up
  (Phase 5 / DOOM milestone territory).
- **ALT+O entry still routes through the dock dispatch.** No
  regressions observed, but the snapshot is now taken inside the
  helper, not at the chord handler. That's the intended scope
  boundary — chord handlers stay thin, the helper owns state
  bracketing.
- **Nested OpenGEM launches are safe.** `desktop_snapshot` is
  stack-allocated, so each re-entry captures/restores its own
  focus value. This is conservative; no real flow re-enters, but
  the contract is clean.

### Next phase inputs
- Phase 4 (OPENGEM-004 App Catalog) benefits from the stable
  snapshot contract: it can rely on focus being restored after any
  catalog-driven launch, not just OpenGEM.
- Phase 5 (OPENGEM-005 Input/Mouse) will add `[ mouse ] opengem
  session: cursor disabled|restored` markers that logically pair
  with the Phase 3 `[ ui ] opengem overlay active|dismissed`
  vocabulary.
- Phase 6 (OPENGEM-006 DOOM) requires the state-save contract so
  the dock is coherent when DOOM exits back to the desktop via
  OpenGEM.
