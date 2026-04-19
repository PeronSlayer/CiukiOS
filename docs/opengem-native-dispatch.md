# OpenGEM native absolute-path dispatcher (OPENGEM-014)

## Context
OPENGEM-013 delivered the preload probe that actually reads GEM.EXE bytes from the absolute path and publishes a verdict. OPENGEM-014 promotes the verdict from observability to **real dispatch**: when the preload confirms a BAT or COM target, CiukiOS skips the historical `shell_run()` path (which normalizes the name and searches CWD + fallback roots) and invokes the interpreter/runtime directly on the resolved absolute path.

16-bit MZ (GEM.EXE) is intentionally out of scope here — it requires a DOS extender or v8086 monitor which is an OPENGEM-015+ architectural effort.

## Contract changes

### Preload out-params (new)
`stage2_opengem_preload_absolute()` now accepts three out-params:
```c
static int stage2_opengem_preload_absolute(
    const char *path, u32 expect_size, const char *classify,
    const char **out_verdict,
    const char **out_reason,
    u32 *out_read_bytes);
```
The caller can inspect the emitted verdict/reason without re-parsing the serial stream.

### Verdict promotion
Inside the preload, the bat and com branches now emit:
```
OpenGEM: preload verdict=dispatch-native reason=bat-interp-ready
OpenGEM: preload verdict=dispatch-native reason=com-runtime-ready
```
MZ / signature-mismatch / unsupported-* / preload-* paths keep emitting `defer-to-shell-run` (reason tokens unchanged and frozen).

### Native dispatcher marker set (frozen, append-only)
```
OpenGEM: native-dispatch begin path=<p> kind=<bat|com> reason=<r>
OpenGEM: native-dispatch <kind>=<invoked|failed>
OpenGEM: native-dispatch complete errorlevel=<n>
```

### Dispatch behavior
| Preload verdict/reason | Action |
|------------------------|--------|
| `dispatch-native / bat-interp-ready` | `shell_run_batch_file(boot_info, handoff, found_path)` |
| `dispatch-native / com-runtime-ready` | `shell_run_staged_image(boot_info, handoff, basename, read_bytes, "")` (buffer already staged by preload) |
| `defer-to-shell-run / *` | fall through to `shell_run(boot_info, handoff, found_path)` |

The COM branch reuses the bytes the preload already placed at `SHELL_RUNTIME_COM_ENTRY_ADDR`, eliminating the double-I/O flagged as a risk in OPENGEM-013.

## Files touched
- `stage2/src/shell.c` — preload out-params + new `stage2_opengem_dispatch_native()` + call-site branching.
- `scripts/test_opengem_native_dispatch.sh` — new gate (20 OK / 0 FAIL).
- `Makefile` — target `test-opengem-native-dispatch`.

## Gate assertions (static)
- Sentinel `OPENGEM-014` and helper `stage2_opengem_dispatch_native`.
- Preload exposes `out_verdict` / `out_reason` / `out_read_bytes`.
- All four dispatcher marker variants present (`begin`, `bat=invoked`, `com=invoked`, `com=failed`, `complete errorlevel=`).
- Bat branch promotes verdict to `dispatch-native`; com branch likewise.
- MZ branch still carries `mz-16bit-pending` + `defer-to-shell-run`.
- Real execution calls: `shell_run_batch_file(boot_info, handoff, path)` and `shell_run_staged_image(boot_info, handoff, basename, read_bytes, …)`.
- Call-site ordering `preload → dispatch_native`, with `shell_run()` reachable via the else branch.
- Dispatcher internal marker ordering (serial_write lines only): `begin < (bat|com)= < complete`.
- Makefile target declared.

## Runtime (opt-in via `CIUKIOS_OPENGEM_BOOT_LOG`)
- Preload marker emits `verdict=dispatch-native reason=(bat-interp-ready|com-runtime-ready)`.
- Native-dispatch begin/invoked/complete markers well-formed.

## Risks
- `shell_run_batch_file()` does its own `fat_read_file()`, so BAT still sees two reads (preload + interpreter). Acceptable — BAT payloads are tiny text.
- `shell_run_staged_image()` trusts the preload-staged buffer; if future changes introduce a code path that mutates `SHELL_RUNTIME_COM_ENTRY_ADDR` between preload and dispatch, COM behavior would silently corrupt. Mitigation: the two calls are adjacent in `shell_run_opengem_interactive()` and there is no intervening FAT I/O.
- Skipping `shell_run()` for bat/com means OPENGEM-014 does **not** emit the historical `[ dosrun ]` argv/ok/error markers for these paths. The native dispatcher owns a disjoint marker set, and downstream gates (`test-bat-interp`, `test-opengem-dispatch`) remain green because they still exercise the shell-command entry points which go through the traditional path.
- Verdict literal `dispatch-native` is now actively emitted; any downstream consumer that assumed it was "reserved but absent" needs to treat it as a real event.

## Next step
- OPENGEM-015: deep MZ parse on the absolute path. Extract `e_cs`, `e_ip`, `e_ss`, `e_sp`, `e_minalloc`, `e_maxalloc`, relocation count, load-image size. Publish a viability verdict for GEM.EXE explicitly (DPMI-required vs runnable-in-real-mode). Still observability; no execution.
- OPENGEM-016+: 16-bit execution subsystem (v8086 monitor or DPMI server). This is the real blocker for gem.exe native dispatch and is the first multi-session architectural milestone beyond the OpenGEM observability series.
