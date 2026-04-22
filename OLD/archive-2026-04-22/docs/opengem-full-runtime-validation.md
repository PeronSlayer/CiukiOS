# OpenGEM Full Runtime Validation (OPENGEM-007)

This document defines the runtime contract that separates a
"preflight-only" OpenGEM launch from a real, desktop-visible session.
Phase 6 closed the integration path (catalog discovery, launcher
button, BAT interpreter, mouse quiesce, escape chord). Phase 7
closes the **observability gap**: the boot log now contains
distinct, ordered markers that a runtime gate can assert.

## Marker vocabulary

### New granular runtime markers (OPENGEM-007)
```
OpenGEM: runtime handoff begin
OpenGEM: desktop first frame presented
OpenGEM: interactive session active
OpenGEM: runtime session ended
```

### Historical markers preserved (backward-compat)
```
OpenGEM: boot sequence starting      (OPENGEM-001, stage2.c at init)
OpenGEM: launcher window initialized (OPENGEM-001, shell.c on pass)
OpenGEM: exit detected, returning to shell (OPENGEM-001)
[ app ] opengem preflight passed     (OPENGEM-001)
[ app ] opengem launch completed     (OPENGEM-001)
[ ui ] opengem dock state saved: sel=<n>         (OPENGEM-003)
[ ui ] opengem overlay active                    (OPENGEM-003)
[ ui ] opengem overlay dismissed, state restored (OPENGEM-003)
[ mouse ] opengem session: cursor disabled       (OPENGEM-005)
[ mouse ] opengem session: cursor restored       (OPENGEM-005)
[ mouse ] opengem hook installed                 (OPENGEM-005)
[ kbd ] opengem escape chord: alt+g+q detected   (OPENGEM-005)
```

## Expected emission order

The runtime gate asserts the following ordering inside
`shell_run_opengem_interactive()`:

```
1. [ ui ] opengem dock state saved: sel=<n>
2. [ app ] opengem preflight passed
3. OpenGEM: launcher window initialized
4. [ ui ] opengem overlay active
5. stage2_mouse_opengem_session_enter()
      -> [ mouse ] opengem session: cursor disabled
6. OpenGEM: runtime handoff begin            <-- OPENGEM-007 (new)
7. OpenGEM: desktop first frame presented    <-- OPENGEM-007 (new)
8. OpenGEM: interactive session active       <-- OPENGEM-007 (new)
9. shell_run()  (BAT/EXE/COM dispatch, user session)
10. OpenGEM: runtime session ended           <-- OPENGEM-007 (new)
11. stage2_mouse_opengem_session_exit()
      -> [ mouse ] opengem session: cursor restored
12. OpenGEM: exit detected, returning to shell
13. [ app ] opengem launch completed
14. [ ui ] opengem overlay dismissed, state restored
```

The gate script enforces:
- `runtime handoff begin`, `desktop first frame presented`, and
  `interactive session active` must appear **after**
  `stage2_mouse_opengem_session_enter()` and **before**
  `shell_run(boot_info, handoff, found_path);`.
- `runtime session ended` must appear **after** `shell_run()` and
  **before** `stage2_mouse_opengem_session_exit()`.

## Validation modes

### Static (default — always runs)
```
make test-opengem-full-runtime
```
Asserts:
1. All four new markers are present in `stage2/src/shell.c`.
2. All preserved historical markers are still present.
3. The ordering contract above is enforced by two AWK probes.
4. The Makefile target is declared.

### Runtime (opt-in)
```
export CIUKIOS_OPENGEM_BOOT_LOG=/path/to/stage2-boot.log
make test-opengem-full-runtime
```
Additionally asserts that the four OPENGEM-007 runtime markers
appear in the captured boot log. Default path is
`.ciukios-testlogs/stage2-boot.log`; a custom path can be supplied
via `CIUKIOS_OPENGEM_BOOT_LOG`.

The gate cleanly SKIPs the runtime assertions when no boot log is
available.

## State restore and escape chord

The ALT+G+Q exit chord (OPENGEM-005) remains the canonical way to
terminate an OpenGEM session from the desktop scene. The gate does
not assert the full chord round-trip because that requires a real
QEMU run; it does assert that:
- `[ kbd ] opengem escape chord: alt+g+q detected` is still
  emitted by the desktop handler.
- `stage2_mouse_opengem_session_exit()` is still wired after
  `shell_run()` so the fallback cursor is always restored.
- Launcher focus restore via `ui_set_launcher_focus()` (from
  OPENGEM-003) remains in place on both success and preflight-fail
  paths.

## Risks and follow-ups

- **Runtime assertions are opt-in.** On macOS CI the gate stays
  static-only because `make test-stage2` / `make test-fallback`
  require a Linux toolchain (tracked in
  `/memories/repo/ciukios-build-notes.md`). The markers are
  structurally identical whether emitted statically or at runtime,
  so drift is low.
- **`desktop first frame presented` is a semantic marker, not a
  functional guarantee.** It is emitted at the moment stage2 hands
  control to `shell_run()` for the OpenGEM entry point. A
  DOS-native OpenGEM build that fails to blit its first frame
  (e.g. mode-13 mismatch) will still produce the marker. A future
  phase can wire a real "first frame detected" hook via the
  existing INT 33h/INT 10h reflector once that path is a source
  of truth.
- **No ABI break.** `ciuki_services_t` and the loader handoff
  remain frozen.
