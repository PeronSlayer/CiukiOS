# OPENGEM-013 — Absolute-path preload probe

## Context and goal
OPENGEM-012 delivered lexical classification but no file I/O. OPENGEM-013 closes that gap: CiukiOS now actually reads the resolved binary from its absolute path via `fat_read_file()` and publishes a dispatch verdict based on the real on-disk signature. Execution remains delegated to `shell_run()` — native dispatch lands in OPENGEM-014.

## Files touched
- `stage2/src/shell.c` — new `stage2_opengem_preload_absolute()` helper; call site passes the classify label by re-deriving it from `found_path`'s trailing 3 chars (avoids leaking classify's internal state as a module-global).
- `scripts/test_opengem_preload.sh` — new gate (37 OK / 0 FAIL).
- `Makefile` — new target `test-opengem-preload` between `test-opengem-absolute-dispatch` and `test-doom-via-opengem`.
- `docs/opengem-preload.md` — contract.
- `documentation.md` — item 23.

## Decisions
1. **Stage into the real runtime buffer.** `SHELL_RUNTIME_COM_ENTRY_ADDR` is the same buffer `shell_run_from_fat()` uses; writing there costs nothing at correctness level and leaves the bytes in place for OPENGEM-014 to reuse.
2. **Guards first, I/O second.** Check `no-path`, `preload-empty`, `preload-too-large` before calling `fat_read_file()`. Keeps the hot path short for pathological inputs.
3. **2-byte signature peek.** Sufficient to distinguish MZ vs non-MZ. PE/ELF discrimination not needed today.
4. **Cross-check classify label.** The preload sees both the lexical classify label and the real signature, so it can emit `signature-mismatch` if the on-disk bytes contradict the extension.
5. **`dispatch-native` reserved literal.** Static gate already accepts it; no emission path today. This turns OPENGEM-014's verdict flip into a drop-in change without a marker-vocabulary migration.
6. **Classify label re-derivation at call site.** `shell_run_opengem_interactive()` computes the label from `found_path` instead of importing one from `stage2_opengem_classify_absolute()`. Keeps phases independently testable and avoids a shared mutable.

## Validation performed
- `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh` — build OK.
- `make test-opengem-preload` — **37 OK / 0 FAIL**.
- Full regression (14 gate, all PASS):
  - `test-opengem-absolute-dispatch`
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
- Double-I/O: preload reads the file once, `shell_run_from_fat()` re-reads it. Bandwidth waste but no correctness issue. OPENGEM-014 eliminates the redundancy.
- Signature heuristic for `text` is intentionally permissive (first byte printable ASCII). BAT files starting with `@ECHO` or a comment `:` match cleanly; a BAT starting with a UTF-8 BOM would miss and land as `unknown`. Acceptable for FreeDOS-era payloads.
- The classify label used by preload is computed locally; if OPENGEM-012's logic changes, both sites must stay in sync. Gate checks both. Planned fold in OPENGEM-014.

## Next step suggestion
- OPENGEM-014: promote preload from observability to dispatch. When `verdict=defer-to-shell-run reason=bat-interp-ready`, call `shell_run_batch_file()` directly on `found_path` and emit `verdict=dispatch-native reason=bat-interp-ready`. Same for `com-runtime-ready` via `shell_run_staged_image()` on the already-staged buffer. MZ/DOS extender still deferred to OPENGEM-015+.

## Branch + commit
- Branch: `feature/opengem-013-absolute-loader` (from OPENGEM-012 tip `a912bce`).
- Awaiting explicit `fai il merge`. Do not merge into main automatically.
