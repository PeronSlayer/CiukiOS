# Handoff - 2026-04-17 - m6-finish-ciukpm-smoke

## Context and goal
Close the operational baseline for SR-M6-001 by restoring deterministic M6 gate behavior on hosts without usable QEMU serial capture, then add a reproducible M6 smoke executable included in the OS image for run-path validation.

## Files touched
1. Makefile
2. run_ciukios.sh
3. scripts/test_m6_pmode_contract.sh
4. scripts/test_m6_transition_contract_v2.sh
5. scripts/test_doom_readiness_m6.sh
6. scripts/test_m6_dos_program.sh
7. com/m6_smoke/ciukpm.c
8. com/m6_smoke/linker.ld
9. docs/m6-dos-extender-requirements.md
10. Roadmap.md
11. README.md
12. stage2/include/version.h

## Decisions made
1. Restored static marker fallback for M6 runtime gates when QEMU serial capture is unavailable, matching the earlier handoff assumptions.
2. Added CIUKPM.EXE as a reproducible MZ smoke binary using the existing deterministic mkciukmz_exe wrapper pipeline.
3. Kept FreeDOS pipeline drift as non-blocking for aggregate M6 readiness because it is outside the protected-mode readiness baseline.

## Validation performed
1. make all -> PASS
2. make test-m6-pmode -> PASS (static fallback)
3. bash scripts/test_m6_transition_contract_v2.sh -> PASS (static fallback)
4. make test-m6-smoke -> PASS (static fallback)
5. bash scripts/test_doom_readiness_m6.sh -> PASS

## Risks and next step
1. Runtime serial capture remains unreliable on this host; M6 runtime gates still rely on deterministic fallback.
2. CIUKPM.EXE validates image/run wiring and MZ launch path, not real DPMI execution yet.
3. Next step: start a follow-up branch from updated main and add a first DOS/4GW-style smoke contract beyond the current skeleton.
