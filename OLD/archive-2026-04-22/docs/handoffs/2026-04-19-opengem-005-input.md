# 2026-04-19 — OPENGEM-005: Input Routing and Mouse/Keyboard Bridge

## Context and goal
Phase 5 of the OpenGEM UX roadmap
([docs/roadmap-opengem-ux.md](../roadmap-opengem-ux.md)). Harden the
boundary between stage2's fallback cursor/keyboard path and a DOS-
native OpenGEM session: quiesce the mode-13 cursor blitter while
OpenGEM owns the screen, expose an append-only `int33_hooks_t` ABI
so an eventual GEMVDI host-app can install its own INT 33h event
callback, and make the ALT+G+Q escape chord unambiguously
identifiable in boot logs.

No version bump (`CiukiOS Alpha v0.8.7`). No changes to `main`.

## Files touched
- `stage2/include/mouse.h` — append-only ABI: `int33_hooks_t`,
  `STAGE2_INT33_HOOKS_VERSION`, `stage2_mouse_set_opengem_hooks()`,
  `stage2_mouse_opengem_session_{enter,exit}()`,
  `stage2_mouse_opengem_cursor_quiesced()`.
- `stage2/src/mouse.c` — new guarded state `g_opengem_hooks` +
  `g_opengem_cursor_quiesced`; implementations emit the frozen
  serial markers.
- `stage2/src/shell.c` — `shell_run_opengem_interactive()` brackets
  `shell_run()` with `session_enter`/`session_exit`;
  `shell_mouse_draw_cursor_mode13()` gates on
  `stage2_mouse_opengem_cursor_quiesced()`; ALT+G+Q handler emits
  the new `[ kbd ] opengem escape chord: alt+g+q detected` marker.
- `scripts/test_opengem_input.sh` — new host-side static gate
  (27 assertions) + opt-in runtime boot-log probe.
- `Makefile` — new target `test-opengem-input`.
- `docs/roadmap-opengem-ux.md` — Phase 5 → DONE.
- `documentation.md` — new item 16 in Current Project State.
- `docs/handoffs/2026-04-19-opengem-005-input.md` — this file.
- `docs/collab/diario-di-bordo.md` — local entry (gitignored).

## Decisions made
- **Append-only hook struct.** `int33_hooks_t` carries a `version`
  field + three nullable callbacks. Consumers gate against
  `STAGE2_INT33_HOOKS_VERSION` so future tail fields (e.g.
  `on_cursor_show`) can be added without breaking the ABI.
- **Quiesce flag, not teardown.** The mode-13 cursor blitter simply
  returns early while `g_opengem_cursor_quiesced` is set. The
  INT 33h state (position, button mask, show count) is untouched;
  when the session ends, the next blit paints at the preserved
  coordinates. This avoids any "pointer snaps to 0,0" artifact on
  return to the desktop.
- **Session bracket lives in the launcher helper.** Placing
  `session_enter`/`exit` around `shell_run()` inside
  `shell_run_opengem_interactive()` means every OpenGEM invocation
  (normal exit, BAT-aborted exit, keyboard-exited exit) restores
  the cursor. Idempotent guard on the flag makes accidental double
  calls safe.
- **Escape-chord marker is additive.** The existing
  `[ ui ] exit chord alt+g+q triggered` marker stays. The new
  `[ kbd ] opengem escape chord: alt+g+q detected` is emitted
  immediately after, giving telemetry a distinct keyboard-domain
  line without disturbing consumers of the UI marker.
- **Hook install is null-safe.** Passing `NULL` clears the table
  and returns to the default fallback path. Passing a `version <
  STAGE2_INT33_HOOKS_VERSION` also clears (defensive).
  `on_mouse_event` is declared for future use (per-event tap for a
  GEMVDI host-app synthesizing its own INT 33h state); stage2 does
  not currently invoke it because the PS/2 IRQ12 path already
  drives `g_mouse_state` and the INT 33h state.
- **No services ABI extension.** `ciuki_services_t` stays frozen.
  DOS apps that need the INT 33h surface continue to go through
  the existing INT 33h function-code dispatch in shell.c. The new
  hooks target stage2-internal consumers (a future native
  OpenGEM build) and are exposed via the header, not via the
  services vector.

## Validation performed
- `make test-opengem-input` → **PASS** (27 OK / 0 FAIL).
- `make test-opengem-file-browser` → **PASS** (Phase 4 regression).
- `make test-opengem-launch` → **PASS** (Phase 3 regression).
- `make test-bat-interp` → **PASS** (Phase 2 regression).
- `make test-opengem-smoke` → **PASS**.
- `make test-opengem` → **PASS**.
- `make test-gui-desktop` → **PASS**.
- `make test-mouse-smoke` → **PASS** (static fallback).
- `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh` → **PASS**.

`make test-stage2` / `make test-fallback` remain blocked on macOS
per `/memories/repo/ciukios-build-notes.md`; not introduced by this
task.

## Risks and next step
- **INT 33h reflector bypass unexplored.** If a DOS app resets the
  mouse with `INT 33h AX=0000h` while an OpenGEM session is active
  (unlikely but possible), the stage2 reflector will clear the
  INT 33h state and the quiesce flag will still hide the cursor
  until the session ends. Mitigation: acceptable for Phase 5;
  OpenGEM is expected to own the mouse surface for the duration of
  its session.
- **Hook callback preemption.** `on_session_enter` / `on_session_exit`
  are invoked from the same task context as the shell, so they
  must not block indefinitely. No preemption model is promised.
  Documented in `mouse.h`.
- **No runtime validation on macOS.** The boot-log probe is opt-in
  and currently SKIPs. A Linux/CI run populating
  `.ciukios-testlogs/stage2-boot.log` would exercise the runtime
  markers; not introduced by this task.

### Next phase inputs
- Phase 6 (OPENGEM-006 DOOM) can assume:
  - The CiukiOS fallback cursor is quiesced while OpenGEM is
    active, so a DOOM launch via OpenGEM will not have a ghost
    pointer painted on top of the framebuffer.
  - The `[ kbd ] opengem escape chord: alt+g+q detected` marker
    can be used to correlate "user exited DOOM via OpenGEM back
    to desktop" in the boot log.
  - `app_catalog_find("DOOM.EXE")` from Phase 4 is available and
    can emit `[ doom ] catalog discovered DOOM.EXE at <path>`.
