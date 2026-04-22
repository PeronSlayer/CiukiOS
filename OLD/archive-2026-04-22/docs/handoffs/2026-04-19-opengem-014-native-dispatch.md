# OPENGEM-014 — Native absolute-path dispatcher (BAT + COM)

## Context and goal
OPENGEM-013 delivered the preload probe; the verdict was observability-only. OPENGEM-014 actually honors the verdict: for BAT and COM targets, CiukiOS now dispatches directly on the resolved absolute path, bypassing `shell_run()`'s name normalization (which had been silently defeating the OPENGEM-010 probe ordering). MZ (gem.exe) is explicitly deferred — it needs a 16-bit execution layer (OPENGEM-015+).

## Files touched
- `stage2/src/shell.c` — new helper `stage2_opengem_dispatch_native()`; preload exposes verdict/reason/read-bytes via out-params; call-site branches on the verdict.
- `scripts/test_opengem_native_dispatch.sh` — new gate (20 OK / 0 FAIL).
- `Makefile` — target `test-opengem-native-dispatch`.
- `docs/opengem-native-dispatch.md` — contract.
- `documentation.md` — item 24.

## Decisions
1. **Promote dispatch-native literal**. The verdict was reserved in OPENGEM-013; now it is actively emitted for bat/com. Frozen set stays (bat-interp-ready, com-runtime-ready); only the verdict pair flips.
2. **Reuse preload buffer for COM**. Since the preload stages at `SHELL_RUNTIME_COM_ENTRY_ADDR`, the COM path hands the already-staged buffer to `shell_run_staged_image()` — eliminating the double-I/O that OPENGEM-013 called out as a risk.
3. **Re-read for BAT**. `shell_run_batch_file()` does its own I/O into `g_shell_file_buffer`. Not worth refactoring today; BAT payloads are tiny text.
4. **Out-params instead of a struct**. Three pointers (verdict, reason, read_bytes) is simpler than introducing a `typedef struct` for one call site. Easy to extend later.
5. **MZ stays on shell_run()**. Until OPENGEM-015+ delivers a 16-bit execution environment, the `shell_run_staged_image()` MZ path rejects with `[dosrun] mz dispatch=pending reason=16bit`. Routing MZ through the native dispatcher today would just duplicate that rejection.
6. **Disjoint marker set**. Native dispatcher emits `OpenGEM: native-dispatch …` markers; it does NOT emit the historical `[ dosrun ]` argv/ok/error markers because those are `shell_run()`'s contract. Downstream gates that need dosrun markers keep using shell-command entry points.

## Validation performed
- Build: `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh` — OK.
- `make test-opengem-native-dispatch` — **20 OK / 0 FAIL**.
- `make test-opengem-preload` — 37/0 (backward-compatible; the verdict literal is part of the gate vocabulary from day one).
- Full regression (15 gate, all PASS):
  - test-opengem-preload, test-opengem-absolute-dispatch, test-opengem-extender, test-opengem-dispatch, test-opengem-real-frame, test-opengem-full-runtime, test-opengem-smoke, test-opengem-launch, test-opengem-input, test-opengem-file-browser, test-bat-interp, test-doom-via-opengem, test-gui-desktop, test-mouse-smoke, test-opengem.

## Risks
- Downstream consumers that keyed off the observability-only nature of OPENGEM-013 will now see real execution side effects for bat/com absolute paths. Only the OpenGEM interactive entry emits these; the `run` shell command is unchanged.
- The COM branch assumes no intervening FAT I/O between preload and dispatch. Validated by reading the single call site; worth re-validating whenever markers 009..014 move.

## Next step suggestion
- OPENGEM-015: deep MZ parse directly on the absolute-path buffer. Surface real header fields (`e_cs`, `e_ip`, `e_ss`, `e_sp`, relocation count, load-image size) and publish a viability verdict. Still observability.
- OPENGEM-016: design conversation on the 16-bit execution layer (v8086 monitor vs DPMI server). This is the multi-session architectural effort that will finally unblock real native dispatch of gem.exe.

## Branch + commit
- Branch: `feature/opengem-014-native-bat-com` (from OPENGEM-013 tip `4048d76`).
- Awaiting explicit `fai il merge`. Do not merge into main automatically.
