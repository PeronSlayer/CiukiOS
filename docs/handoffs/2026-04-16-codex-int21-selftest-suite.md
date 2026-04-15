# Handoff - INT21 Self-Test Suite + Dedicated Test Script (2026-04-16)

## Branch
- `feature/codex-m2-m4-int21-bios-tests`

## Why this change
Prepare deterministic, non-interactive validation for roadmap X3/X4 work while Claude continues parallel FAT tasks.

## What was added

### 1) Stage2 INT21 baseline self-test (boot-time)
- Added exported function in shell module:
  - `stage2_shell_selftest_int21_baseline()`
- Called during stage2 boot init; emits one clear marker:
  - PASS: `[ test ] int21 priority-a selftest: PASS`
  - FAIL: `[ test ] int21 priority-a selftest: FAIL`

Validated AH paths include:
1. `00h`, `02h`, `09h`, `19h`, `25h`, `30h`, `35h`, `4Ch`
2. Deterministic stubs: `48h`, `49h`, `4Ah`
3. Unsupported function fallback (`CF=1`, `AX=0001h`)

### 2) Stage2 boot test tightened
- `scripts/test_stage2_boot.sh` now requires PASS marker and forbids FAIL marker.

### 3) Dedicated INT21 test script
- New script: `scripts/test_int21_priority_a.sh`
- Checks the stage2 log for:
  - INT21 self-test PASS
  - BIOS compatibility markers (`INT10h`, `INT16h`, `INT1Ah`)
  - absence of fail/fault patterns

### 4) Makefile target
- Added `make test-int21`

## Files changed
- `stage2/include/shell.h`
- `stage2/src/shell.c`
- `stage2/src/stage2.c`
- `scripts/test_stage2_boot.sh`
- `scripts/test_int21_priority_a.sh` (new)
- `Makefile`

## Validation run
All pass on this branch:

1. `make -j4`
2. `make test-stage2`
3. `make test-fallback`
4. `make test-fat-compat`
5. `make test-int21`

## Notes
1. Self-test intentionally skips interactive AH paths that would block CI (`01h/08h` runtime input semantics).
2. This is a deterministic compatibility harness, not yet a full DOS conformance suite.
