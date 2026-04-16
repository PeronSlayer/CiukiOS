# HANDOFF - Phase5/Batch/PMode Execution Pack

## Date
2026-04-17

## Context
User requested direct execution of the first five roadmap priorities:
1) INT21 parity hardening (edge cases + memory ownership metadata)
2) dedicated BIOS compatibility tests (INT10h/INT16h/INT1Ah)
3) startup chain baseline (CONFIG.SYS + AUTOEXEC.BAT)
4) batch parser MVP with env expansion
5) protected-mode transition contract definition and implementation scaffold

## Completed scope
1. INT21 memory ownership metadata was added to memory blocks and enforced for AH=49h/AH=4Ah operations.
2. INT21 baseline selftests now include cross-PSP ownership rejection checks.
3. Dedicated BIOS baseline selftests were added for INT10h/INT16h/INT1Ah and wired to startup serial markers.
4. Shell startup chain now loads CONFIG.SYS and executes AUTOEXEC.BAT when present.
5. Batch MVP implemented with labels, GOTO, IF ERRORLEVEL ... GOTO, REM, SET, ECHO and command dispatch.
6. Environment variable support implemented (`set` command + `%VAR%` expansion).
7. Protected-mode contract v1 documented and surfaced by shell command `pmode` and startup compatibility marker.

## Touched files
1. stage2/src/shell.c
2. stage2/src/stage2.c
3. scripts/test_stage2_boot.sh
4. docs/pmode-transition-contract.md

## Technical decisions
1. Decision:
Use PSP ownership metadata in memory block table for AH=49h/AH=4Ah parity hardening.
Reason:
Prevents foreign-PSP free/resize and better matches DOS ownership semantics trajectory.
Impact:
Improved determinism for multi-process compatibility expectations.

2. Decision:
Implement batch MVP directly in shell runtime using existing FAT file read path and bounded execution limits.
Reason:
Fastest path to unlock AUTOEXEC/BAT workflows without waiting for full COMMAND.COM replacement.
Impact:
Enables immediate startup-chain automation and script-driven flows.

3. Decision:
Represent protected-mode work as explicit contract v1 marker + documentation + runtime visibility.
Reason:
Creates a concrete, testable bridge point for future DOS extender transition work.
Impact:
Reduces ambiguity and prepares next implementation increment.

## ABI/contract changes
1. Internal shell memory block metadata now tracks owner PSP segment.
2. New protected-mode contract document created: docs/pmode-transition-contract.md.

## Tests executed
1. Command:
make all
Result:
PASS

2. Command:
make test-mz-regression
Result:
PASS

3. Command:
make check-int21-matrix
Result:
PASS

## Current status
1. First five roadmap priorities requested by user are implemented at baseline level.
2. Batch/startup/env and PMODE contract are now active scaffolds for the next compatibility layer increments.

## Risks / technical debt
1. Batch MVP is intentionally limited (no full DOS batch grammar yet).
2. COMMAND.COM parity remains incomplete and still requires dedicated phase closure work.
3. Protected-mode contract is scaffold-level; actual mode switch/extender bridge is pending.

## Next steps (recommended order)
1. Expand batch grammar (CALL, IF string compare, SHIFT, argument substitution) for higher script compatibility.
2. Add targeted regression tests for startup-chain behavior with fixture CONFIG/AUTOEXEC payloads.
3. Implement first protected-mode smoke bridge using the v1 contract markers.

## Notes for Claude Code
- Keep `shell_set_errorlevel` authoritative for batch IF ERRORLEVEL behavior.
- Preserve bounded limits (`SHELL_BATCH_MAX_*`) to avoid runaway script loops.
- Extend PMODE contract incrementally without breaking CIUKEX64 marker compatibility.
