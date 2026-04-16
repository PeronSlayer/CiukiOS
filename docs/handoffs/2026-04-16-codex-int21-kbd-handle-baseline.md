# HANDOFF - INT21 Keyboard Status/Flush + Handle Baseline

## Date
`2026-04-16`

## Context
Continue Codex roadmap tasks C1/C2 while Copilot GUI work was merged on `main`.
Target: extend DOS compatibility surface safely with deterministic behavior and full regression coverage.

## Completed scope
1. Added INT21 keyboard compatibility APIs:
   - `AH=0Bh` keyboard status (`AL=00h` or `AL=FFh`)
   - `AH=0Ch` keyboard flush + deterministic follow-up input subset (`AL=01h/08h`)
2. Added keyboard ring-buffer flush primitive used by `AH=0Ch`.
3. Added INT21 handle API deterministic baseline:
   - `AH=3Ch` create (stub: `CF=1 AX=0005h`)
   - `AH=3Dh` open (stub: `CF=1 AX=0002h`)
   - `AH=3Eh` close (std handles 0/1/2 accepted, others `AX=0006h`)
   - `AH=3Fh` read (stdin baseline on handle 0; deterministic errors otherwise)
   - `AH=40h` write (stdout/stderr baseline on handles 1/2; deterministic errors otherwise)
   - `AH=41h` delete (stub: `CF=1 AX=0002h`)
   - `AH=42h` seek (std handles return `DX:AX=0`; others `AX=0006h`)
4. Extended selftest (`stage2_shell_selftest_int21_baseline`) with new coverage.
5. Updated INT21 compatibility docs and matrix validation requirements.
6. Updated INT21 test script to assert the IO/handle compatibility marker.

## Touched files
1. `stage2/include/keyboard.h`
2. `stage2/src/keyboard.c`
3. `stage2/src/shell.c`
4. `docs/int21-priority-a.md`
5. `scripts/check_int21_matrix.sh`
6. `scripts/test_int21_priority_a.sh`

## Technical decisions
1. Decision:
Use deterministic DOS-like stubs for write-path file calls not yet backed by handle table/FAT integration.
Reason:
Keeps behavior stable and testable while avoiding unsafe partial backend.
Impact:
Compatibility increases without introducing silent corruption/regression.

2. Decision:
Allow baseline standard handles (`0,1,2`) immediately for `3Eh/3Fh/40h/42h`.
Reason:
Many DOS binaries rely on stdin/stdout/stderr semantics before full file-handle APIs.
Impact:
Improves runtime behavior for simple tools and COM tests.

3. Decision:
Introduce buffered pending char state for `AH=0Bh` polling + `AH=0Ch` flush flow.
Reason:
Need deterministic keyboard status without losing next character.
Impact:
Predictable input polling path compatible with DOS-like usage patterns.

## ABI/contract changes
1. No UEFI/loader handoff ABI changes.
2. Stage2 internal keyboard API extended with `stage2_keyboard_flush_buffer()`.
3. INT21 semantic surface expanded; all new paths documented in matrix.

## Tests executed
1. `make check-int21-matrix` -> PASS
2. `make test-freedos-pipeline` -> PASS
3. `make test-stage2` -> PASS
4. `make test-fallback` -> PASS
5. `make test-fat-compat` -> PASS
6. `make test-int21` -> PASS

## Current status
1. INT21 baseline now includes keyboard status/flush plus deterministic handle API baseline.
2. Matrix gate enforces documentation for expanded function set.
3. Boot/fallback/FAT/INT21 regressions remain green.

## Risks / technical debt
1. `3Ch/3Dh/41h` remain deterministic stubs until real FAT-backed handle table is implemented.
2. `3Fh`/`40h` buffer pointers currently validated against COM image bounds only (intentional safety-first constraint).
3. `AH=0Ch` currently supports deterministic follow-up subset only.

## Next steps (recommended order)
1. Replace deterministic stubs with real FAT-backed handle table (`3Ch..42h`).
2. Extend `AH=0Ch` dispatch coverage beyond `AL=01h/08h` as needed by target binaries.
3. Add dedicated INT21 handle semantics script (open/read/write/seek/error mapping scenarios).

## Notes for Claude/Copilot/Codex
1. Preserve deterministic `CF/AX` mappings when adding real backends.
2. Keep matrix + gate updated in same commit for each INT21 expansion.
3. Avoid broad refactors in `shell.c` until handle table backend is introduced.
