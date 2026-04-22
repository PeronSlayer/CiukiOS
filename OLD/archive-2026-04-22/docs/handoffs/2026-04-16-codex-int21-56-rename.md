# HANDOFF - INT21 AH=56h Rename + FAT E2E Extension

Date: 2026-04-16
Owner: Codex
Branch: `feature/codex-int21-findfirst-next-v1`

## Scope
Implemented DOS-compatible rename syscall subset in stage2 INT21 dispatcher:
- `AH=56h` (rename/move entry)
- Supports same-directory rename subset only
- Uses `DS:DX` source path and `ES:DI` destination path
- FAT-backed path translation and deterministic DOS-like error mapping

## What Changed
1. `stage2/src/shell.c`
- Added helper: `shell_int21_read_asciiz_seg(...)` to read ASCIIZ from segment:offset with PSP-segment guard.
- Added INT21 handler branch for `AH=56h`:
  - Validates FAT readiness
  - Reads old/new DOS paths from `DS:DX` and `ES:DI`
  - Canonicalizes paths and splits parent/name
  - Rejects wildcard source/target in this subset
  - Rejects cross-directory rename in this subset
  - Ensures source exists, destination does not exist (unless same path)
  - Calls `fat_rename_entry(old_path, new_name)`
  - Returns DOS-like `CF/AX` errors (`0002h`, `0003h`, `0005h`) and success `AX=0000h`

2. `stage2/src/shell.c` (selftest extension)
- Extended `stage2_shell_selftest_int21_fat_handles()` to include rename path:
  - Create/write/read/attr sequence unchanged
  - New rename check: `I21E2E.TXT -> I21E2R.TXT` via `AH=56h`
  - Verifies old path gone and new path present in FAT
  - Deletes renamed file and confirms cleanup
  - Fail-path cleanup now removes both old and renamed canonical files

3. `stage2/src/stage2.c`
- Updated compatibility marker to include rename:
  - `[ compat ] INT21h FAT-backed file handles ready (AH=3Ch/3Dh/3Eh/3Fh/40h/41h/42h/43h/56h)`

4. Validation/docs sync
- `scripts/test_stage2_boot.sh`: updated required marker string with `56h`
- `docs/int21-priority-a.md`:
  - Added implemented item for `AH=56h`
  - Added matrix row for `56h`
- `scripts/check_int21_matrix.sh`:
  - Added `56h` to required matrix functions list

## Verification
Executed on branch:
1. `make check-int21-matrix` -> PASS
2. `make test-stage2` -> PASS
3. `make test-int21` -> PASS
4. `make test-freedos-pipeline` -> PASS
5. `make test-opengem` -> PASS (non-blocking launch markers still WARN as before)

## Notes / Constraints
- `AH=56h` currently enforces PSP-segment path pointers (`DS`/`ES` must match active PSP segment) for deterministic runtime safety.
- Cross-directory rename remains intentionally unsupported in this compatibility phase.
- Wildcard rename templates are intentionally unsupported in this phase.

## Suggested Next Step
- Add a focused INT21 rename selftest (`56h`) for failure mappings:
  - destination already exists
  - source missing
  - cross-directory target
  - wildcard source/target rejection
