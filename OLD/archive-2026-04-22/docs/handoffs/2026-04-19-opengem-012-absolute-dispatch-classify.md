# OPENGEM-012 — Absolute-dispatch classification probe

## Context and goal
OPENGEM-011 closed out the extender readiness observability. OPENGEM-012 adds the classification layer: for the absolute path resolved by OPENGEM-010, publish an explicit capability verdict so a gate (and, later, a real dispatcher) can decide whether to attempt native execution or defer to the historical `shell_run()` path. No file bytes are loaded; this is still pure observability.

## Files touched
- `stage2/src/shell.c` — new `stage2_opengem_classify_absolute()` + `shell_write_u32_hex()` helper; `found_size` captured in the preflight probe; invocation between OPENGEM-011 extender probe and `shell_run()`.
- `scripts/test_opengem_absolute_dispatch.sh` — new gate (24 OK / 0 FAIL).
- `Makefile` — target `test-opengem-absolute-dispatch` between `test-opengem-extender` and `test-doom-via-opengem`.
- `docs/opengem-absolute-dispatch-classify.md` — contract.
- `documentation.md` — item 22.

## Decisions
1. **Extension-only classification.** Lexical, not content-based. Fast, no I/O. The marker explicitly carries `by=path` so a future `by=bytes` variant can co-exist.
2. **Stable reason tokens.** Six fixed tokens kebab-case, machine-readable. Added explicit `no-path` for the degenerate case.
3. **Size from preflight `probe.size`.** Avoids a second FAT directory lookup and renders as 8-digit lowercase hex via the new helper.
4. **Advisory return.** Classifier returns `capable` but `shell_run_opengem_interactive()` ignores it; OPENGEM-013 will promote it to a real gate.

## Validation performed
- `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh` — build OK.
- `make test-opengem-absolute-dispatch` — **24 OK / 0 FAIL**.
- Full regression (13 gate, all PASS):
  - `test-opengem-extender`
  - `test-opengem-dispatch`
  - `test-opengem-real-frame`
  - `test-opengem-full-runtime`
  - `test-opengem-smoke`
  - `test-opengem-launch`
  - `test-opengem-input`
  - `test-opengem-file-browser`
  - `test-bat-interp`
  - `test-doom-via-opengem`
  - `test-gui-desktop`
  - `test-mouse-smoke`
  - `test-opengem`

## Risks
- Lexical classification can be fooled by renamed files. Acceptable given the shipped FreeDOS payload layout; surfaced via `by=path` qualifier.
- Reason tokens are now a public contract; any change requires a documented deprecation path.

## Next step suggestion
- OPENGEM-013: real absolute-path loader. Add `shell_run_from_absolute_path()` that reads via `fat_read_file(found_path, ...)` directly and respects the OPENGEM-012 capability flag. For `capable=1` paths, dispatch natively; for `capable=0 reason=16bit-mz-extender-pending`, wire the DPMI-stub entry point from OPENGEM-011 into a synthesized boot attempt.

## Branch + commit
- Branch: `feature/opengem-012-absolute-dispatch` (from OPENGEM-011 tip `472b111`).
- Awaiting explicit `fai il merge`. Do not merge into main automatically.
