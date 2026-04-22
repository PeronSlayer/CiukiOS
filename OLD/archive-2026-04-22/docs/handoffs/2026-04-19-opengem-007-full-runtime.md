# 2026-04-19 — OPENGEM-007: Full Runtime Visual Launch

## Context and goal
Close the observability gap between the Phase 1–6 integration work
and the actual runtime lifecycle of an OpenGEM session. Phases 1–6
left the launcher, BAT interpreter, catalog, mouse/keyboard bridge
and DOOM path in place; Phase 7 guarantees the boot log carries
four distinct, ordered markers that let a runtime gate classify a
real desktop-visible launch vs. a preflight-only pass, and preserves
every historical marker for backward compatibility.

No version bump (`CiukiOS Alpha v0.8.7`). No changes to `main`.

## Files touched
- `stage2/src/shell.c` — `shell_run_opengem_interactive()` emits
  four new granular markers bracketing the `shell_run()` call.
- `scripts/test_opengem_full_runtime.sh` — new two-mode gate
  (static always + opt-in runtime boot-log probe).
- `Makefile` — new target `test-opengem-full-runtime`.
- `docs/opengem-full-runtime-validation.md` — new contract doc
  (marker vocabulary, expected emission order, validation modes).
- `documentation.md` — item 18 in Current Project State.
- `docs/handoffs/2026-04-19-opengem-007-full-runtime.md` — this.
- `docs/collab/diario-di-bordo.md` — local entry.

## Decisions made
- **Four markers, not three.** The task listed four (`runtime
  handoff begin`, `desktop first frame presented`, `interactive
  session active`, `runtime session ended`). All four are
  emitted; the first three bracket the `shell_run()` call entry,
  the fourth marks its return.
- **Ordering enforced in the gate.** Two AWK probes assert that
  (a) the three "enter" markers appear between
  `stage2_mouse_opengem_session_enter()` and
  `shell_run(boot_info, handoff, found_path);`, and (b) the
  "ended" marker appears between `shell_run()` and
  `stage2_mouse_opengem_session_exit()`. This locks the narrative
  order in the boot log.
- **Backward-compat preserved.** The gate re-asserts every
  historical marker that prior gates rely on (`launcher window
  initialized`, `exit detected, returning to shell`, overlay and
  mouse markers, ALT+G+Q). Any future removal will fail the
  gate.
- **Markers are semantic, not instrumented.** `desktop first
  frame presented` is emitted as stage2 hands control to
  `shell_run()` for the OpenGEM entry point. Instrumenting the
  actual first mode-13 blit would require a hook through the
  INT 10h/INT 33h reflector and is out of Phase 7's scope.
  Documented as a follow-up.
- **Static gate first, runtime opt-in.** macOS CI cannot boot
  QEMU reliably; the gate passes on source invariants plus
  ordering. When a user captures a real boot log
  (`CIUKIOS_OPENGEM_BOOT_LOG=<path>`), the same markers are
  re-asserted against the log.
- **No ABI change.** `ciuki_services_t` frozen; only
  `serial_write()` emissions and a new shell-internal ordering.

## Validation performed
- `make test-opengem-full-runtime` → **PASS** (14 OK / 0 FAIL).
- `make test-opengem-smoke` → **PASS**.
- `make test-opengem-launch` → **PASS**.
- `make test-opengem-input` → **PASS**.
- `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh` → **PASS**.

`make test-stage2` / `make test-fallback` remain blocked on macOS
per `/memories/repo/ciukios-build-notes.md`; not introduced here.

## Risks and next step
- **"First frame presented" is not a functional guarantee.** The
  marker fires even if a DOS-native OpenGEM build fails to blit
  its actual first frame (e.g. mode-13 mismatch). Mitigation:
  future hook through the mode-13 first-blit reflector can gate
  a stricter "frame-real" marker; not blocking today.
- **Runtime assertions are opt-in on macOS CI.** The static path
  enforces presence + ordering; runtime presence requires a
  captured boot log.
- **Marker text frozen as contract.** Any downstream tool
  consuming these strings (logging, telemetry dashboards) should
  treat them as append-only; changing the substrings would break
  both the gate and external consumers.

### Next phase inputs
- Phase 7 gives Phase 8+ (whichever consumer lands next) a solid
  boot-log classification surface. Candidate follow-ups:
  - Instrument a true "first-blit" hook behind a `CIUKIOS_REAL_FRAME=1`
    flag and emit an extra marker `OpenGEM: desktop frame blitted`
    for fixture-gated validation.
  - Add a session duration line
    `OpenGEM: runtime session duration=<ms>` between the
    "session ended" and the mouse teardown for regression
    budgeting.
