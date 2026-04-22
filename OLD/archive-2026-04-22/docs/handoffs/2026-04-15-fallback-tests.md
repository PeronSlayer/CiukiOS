# HANDOFF - fallback boot tests

## Date
`2026-04-15`

## Context
Request: add automatic tests to verify loader fallback behavior when `stage2.elf` is not present in FAT image.

## Completed scope
1. Added runtime flag `CIUKIOS_SKIP_STAGE2=1` in `run_ciukios.sh` to skip copying `stage2.elf`.
2. Created fallback test script `scripts/test_kernel_fallback_boot.sh`.
3. Added Make targets:
   - `test-fallback`
   - `test-boot` (suite: stage2 + fallback).
4. Updated progress tracking in `docs/phase-0-kickoff.md`.

## Touched files
1. `run_ciukios.sh`
2. `scripts/test_kernel_fallback_boot.sh`
3. `Makefile`
4. `docs/phase-0-kickoff.md`

## Technical decisions
1. Decision: drive fallback test via env var (`CIUKIOS_SKIP_STAGE2=1`).
Reason: simple and repeatable, no post-build image surgery.
Impact: same boot script can validate both paths.

2. Decision: keep timeout as nominal test result.
Reason: system enters halt loop in current scaffolding.
Impact: `124` treated as operational success.

## ABI/contract changes
None.

## Tests executed
1. `make test-fallback`
Result: PASS.

2. `make test-boot`
Result: PASS (both stage2 path and kernel fallback path).

## Current status
1. Stage2 boot path is automatically tested.
2. Kernel fallback path is automatically tested.
3. Aggregate suite available via `make test-boot`.

## Risks / technical debt
1. Tests depend on fixed log strings.
2. Stage2 functional validation is still minimal beyond checkpoints.

## Next steps (recommended order)
1. Add `make ci` target calling `test-boot`.
2. Start minimal `GDT/IDT` setup in stage2 with testable markers.
3. Extend tests to ensure `entry bytes` are non-zero in both stage2 and fallback paths.

## Notes for Claude Code
Do not change critical log messages (`required_patterns`) without updating test scripts in `scripts/`.
