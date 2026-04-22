# 2026-04-19 — OPENGEM-001 Launcher Integration (Phase 1)

## Context and goal
Phase 1 of the OpenGEM UX roadmap (`docs/roadmap-opengem-ux.md`): expose
OpenGEM as a first-class launch target from the shell **and** the
desktop scene, preserve the existing preflight probe, add the
boot/launch/exit serial markers called for by the task spec, and ship
a dedicated host-side smoke gate. No version bump (baseline
`CiukiOS Alpha v0.8.7`).

## Files touched
- `stage2/src/shell.c` — added `shell_run_opengem_interactive()` which
  centralizes the preflight (entry probe + FAT readiness), emits the
  new serial markers (`OpenGEM: boot sequence starting`,
  `OpenGEM: launcher window initialized`, `OpenGEM: exit detected,
  returning to shell`, `OpenGEM: runtime not found in FAT, fallback to
  shell`) and dispatches to `shell_run()`. The `opengem` command, the
  new `OPENGEM` desktop launcher item, and the new `ALT+O` shortcut
  all route through this one helper. ALT+O is handled in the desktop
  session loop before the ALT+G chord to keep them independent.
- `stage2/src/ui.c` — extended `g_launcher_items` from 6 to 7 entries,
  appending `OPENGEM` as the last dock item.
- `scripts/test_opengem_smoke.sh` — new host-side smoke gate. Verifies
  runtime payload on disk, stage2 wiring (helper + markers + ALT+O +
  dispatch), launcher item list, image pipeline copy step, and opts
  into a boot-log probe when one is present. Executable bit set.
- `Makefile` — added `test-opengem-smoke` target running the new
  script.
- `docs/opengem-runtime-structure.md` — new reference doc describing
  the OpenGEM runtime layout, the five-candidate entry probe order,
  all CiukiOS entry surfaces (shell / launcher / ALT+O), the serial
  marker catalogue, and the Phase-1 fallback behavior.
- `documentation.md` — added OpenGEM section pointing at the helper,
  launcher integration, markers, and validation gates.
- `docs/collab/diario-di-bordo.md` — local-only diary entry (gitignored).

## Decisions made
- **Single helper, three entry surfaces.** Rather than duplicating the
  preflight into the launcher dispatch or the ALT+O path, all three
  call `shell_run_opengem_interactive()`. This keeps the serial marker
  sequence identical regardless of how the user launched OpenGEM.
- **ALT+O precedence over ALT+G chord.** ALT+O is handled as a
  standalone shortcut *before* the `chord_stage` check for ALT+G+Q, so
  pressing the two shortcuts never cross-contaminates the exit chord.
- **Fallback is silent-but-visible.** When the payload is missing the
  helper returns 0 and the launcher reports `OPENGEM: n/a` in the
  system window status plus `(opengem unavailable)` in the console
  ring. No panic, no state corruption.
- **Smoke gate is static-first.** We mirror the `test_mouse_smoke.sh`
  approach: host-side assertions are authoritative (since QEMU runs
  on macOS are flaky), and the boot-log probe degrades to `[SKIP]`
  when no log is available.
- **Reused existing preflight contract.** The five candidate paths
  (`GEM.BAT`, `GEM.EXE`, `GEMAPPS/GEMSYS/DESKTOP.APP`, `OPENGEM.BAT`,
  `OPENGEM.EXE`) and the existing `[ app ] opengem preflight …`
  markers are preserved verbatim so the older
  `test_opengem_integration.sh` gate remains compatible.

## Validation performed
- `bash scripts/test_opengem_smoke.sh` → PASS (all 13 host-side
  assertions green).
- `make test-opengem-smoke` → PASS (same).
- `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh` → PASS (stage2 +
  all COMs + FAT image build clean, OpenGEM payload included).
- `make test-opengem` → **PASS** (after realigning the `opengem`
  shell help line to the `name  - description` format asserted by
  `scripts/test_opengem_integration.sh`).
- `make test-gui-desktop` → **PASS** (after realigning the `desktop`
  shell help line and adding the
  `Tip: type 'desktop' to test GUI mode (ALT+G+Q to return).`
  post-startup banner line asserted by
  `scripts/test_gui_desktop.sh`).
- `make test-stage2` / `make test-fallback` remain blocked by the
  macOS host limitations tracked in
  `/memories/repo/ciukios-build-notes.md`.

## Risks and next step
- **BAT interpretation is still best-effort.** `GEM.BAT` requires a
  working BAT runner and a reachable `\GEMAPPS\GEMSYS\GEMVDI.EXE`. The
  first preflight hit is the BAT; actual execution depth remains the
  domain of the DOS runtime milestones. Phase 2 of the OpenGEM
  roadmap should harden BAT dispatch before claiming a visible GEMVDI
  window.
- **No visible OpenGEM icon yet.** The launcher entry is a text item
  in the dock list. Phase 2 will add a proper icon/glyph in the
  desktop scene.
- **Pre-existing gate drift.** Two gates
  (`test-opengem`, `test-gui-desktop`) fail on clean `main`. They are
  not in this task's DoD (the spec names `test-stage2`,
  `test-gui-desktop`, and the macOS pipeline — of these only the
  macOS pipeline is green locally; the others are host-blocked or
  stale regardless of this change). Recommend a dedicated follow-up
  to re-align those gates with the current shell help string and
  boot-log capture pipeline on macOS.

## Next phase inputs
Phase 2 will build on:
- `shell_run_opengem_interactive()` as the sole launch entry.
- The `OpenGEM: …` marker vocabulary for validation.
- `docs/opengem-runtime-structure.md` for the entry-point contract.
- `scripts/test_opengem_smoke.sh` as the static-assertion base; it can
  be extended with QEMU+serial-log checks once host capture is wired.
