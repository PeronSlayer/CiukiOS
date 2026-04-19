# OpenGEM absolute-dispatch classification (OPENGEM-012)

## Context
OPENGEM-011 wired the DOS extender readiness probe. The natural
next step for OPENGEM-012 was to actually dispatch `GEM.EXE` from
its absolute path, but doing so requires a real 16-bit DOS extender
(`dos_mz_build_loaded_image` / `shell_run_staged_image` currently
rejects bare 16-bit MZ executables with
`[dosrun] mz dispatch=pending reason=16bit`).

OPENGEM-012 therefore delivers the **classification layer** — the
observability surface that lets a gate assert whether the current
build is expected to run the resolved binary natively, or defer to
the historical `shell_run()` fallback. It is the last step before a
real absolute-path loader lands (OPENGEM-013+).

## Change summary

1. **New helper** `stage2_opengem_classify_absolute(path, size)` in
   `stage2/src/shell.c`:
   - Uses the FAT directory-entry size already captured by the
     preflight `fat_find_file()` call — **no file bytes are read**.
   - Classifies by path extension only (ASCII-fold of trailing 3
     chars) into one of: `mz`, `bat`, `com`, `app`, `unknown`.
   - Publishes a capability verdict (`capable=0|1`) with a stable
     reason token.
   - Returns the capability flag; callers currently cast to `void`.

2. **Frozen marker set** (append-only):
   ```
   OpenGEM: absolute dispatch begin path=<p> size=0x<hex32>
   OpenGEM: absolute dispatch classify=<mz|bat|com|app|unknown> by=path
   OpenGEM: absolute dispatch capable=<0|1> reason=<token>
   OpenGEM: absolute dispatch complete
   ```

3. **Stable reason tokens**:
   | Token | Meaning |
   |-------|---------|
   | `16bit-mz-extender-pending` | MZ found, extender not yet implemented |
   | `bat-interp-available` | BAT delegated to existing BAT interpreter |
   | `com-runtime-available` | COM delegated to existing COM runtime |
   | `no-loader-for-app` | `.APP` not yet supported |
   | `unknown-extension` | fell through kind ladder |
   | `no-path` | preflight did not resolve a path |

4. **Invocation**: `shell_run_opengem_interactive()` calls the
   classify helper immediately after the OPENGEM-011 extender probe
   and before `shell_run()`. Ordering is thus:
   `dispatch target` → `extender probe` → `classify` → `shell_run`.

5. **Small helper** `shell_write_u32_hex()` — 8-digit hex
   formatter used to render the size word; mirrors the OPENGEM-011
   `shell_write_u16_hex()` pattern.

6. **Preflight size capture**: `shell_run_opengem_interactive()`
   now stores `found_size = probe.size` when the matching path is
   found, avoiding a second FAT lookup for the classify probe.

## Files touched
- `stage2/src/shell.c` — classify helper + u32 hex helper +
  invocation + preflight size capture.
- `scripts/test_opengem_absolute_dispatch.sh` — new gate
  (24 OK / 0 FAIL).
- `Makefile` — target `test-opengem-absolute-dispatch`.

## Gate assertions (static)
- OPENGEM-012 sentinel.
- Classify function + u32 hex helper declared.
- All four marker prefixes + `size=0x` + ` by=path` tokens.
- All five classify labels (`mz`, `bat`, `com`, `app`, `unknown`).
- All six reason tokens (stable contract).
- Invocation ordering `extender → classify → shell_run`.
- First-occurrence marker order `begin < classify < capable < complete`.
- `found_size = probe.size` captured at the match.
- Makefile target declared.

## Runtime (opt-in via `CIUKIOS_OPENGEM_BOOT_LOG`)
- `begin … size=0x<8-digit-hex>` well-formed.
- `classify=<label> by=path` valid.
- `capable=<0|1> reason=<token>` well-formed.

## Risks
- Classification by extension is **lexical only**. A file named
  `GEM.EXE` that is actually a PE / Linux ELF / something else will
  still be classified as `mz`. This is acceptable given the
  FreeDOS payload shape we ship, and is surfaced as a known
  limitation (`by=path`).
- The `no-loader-for-app` reason exists for future compatibility
  with GEM `.APP` files; today no code path resolves one.
- The `capable` flag is **advisory**. Consumers still go through
  `shell_run()` until OPENGEM-013+ promotes the flag to a gate.

## Next step
- OPENGEM-013: real absolute-path loader. Candidates:
  1. Wire a new `shell_run_from_absolute_path()` that uses
     `fat_read_file()` directly on `found_path` (bypassing
     `build_run_path()` / CWD / fallback roots).
  2. Honor the OPENGEM-012 capability flag: if `capable=1`, use
     the new loader; if `capable=0 reason=16bit-mz-extender-pending`,
     fall back to the OPENGEM-011 extender probe's DPMI stub for a
     synthesized boot attempt.
