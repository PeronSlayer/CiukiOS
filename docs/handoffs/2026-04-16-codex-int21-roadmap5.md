# Handoff - Codex INT21 Roadmap5 Core
Date: 2026-04-16
Branch: feature/codex-int21-roadmap5-isolated

## Scope completed (5 roadmap tasks)
1. INT21 `AH=06h` direct console I/O added.
2. INT21 `AH=07h` blocking input without echo added.
3. INT21 `AH=0Ah` buffered line input added, plus `AH=0Ch` dispatch support for `AL=0Ah`.
4. INT21 drive state added: `AH=0Eh` set default drive + `AH=19h` returns runtime drive.
5. INT21 DTA APIs added: `AH=1Ah` set DTA + `AH=2Fh` get DTA.

## Runtime details
- Added INT21 state globals for default drive and DTA pointer.
- Added non-blocking keyboard helper for `AH=06h` input mode.
- Buffered line input uses DOS buffer contract at `DS:DX` (`max/count/data...`).
- PSP prep now initializes default DTA to `PSP:0080`.
- Selftest `stage2_shell_selftest_int21_baseline()` extended with deterministic coverage for all new paths.

## Compatibility markers/logs
Added boot marker:
- `[ compat ] INT21h console/dta/drive ready (AH=06h/07h/0Ah/0Eh/1Ah/2Fh)`

## Docs and gates updated
- `docs/int21-priority-a.md`: implemented list + matrix rows updated for new INT21 functions.
- `scripts/check_int21_matrix.sh`: required matrix functions updated.
- `scripts/test_int21_priority_a.sh`: requires new compat marker.
- `scripts/test_stage2_boot.sh`: requires new compat marker; aligned boot assertions with current log flow (`Disk cache ready: lba_count=`).

## Validation run
- `make check-int21-matrix` PASS
- `make test-stage2` PASS
- `make test-int21` PASS
- `make test-fallback` PASS
- `make test-fat-compat` PASS
- `make test-freedos-pipeline` PASS

## Notes for collaborators
- Work executed in isolated worktree due concurrent branch activity.
- FreeDOS runtime binaries were copied locally to run pipeline validation; they are not part of this commit scope unless already tracked in the target branch.
