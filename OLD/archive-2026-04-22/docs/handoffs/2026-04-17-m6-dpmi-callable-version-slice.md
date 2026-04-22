# 2026-04-17 - M6 DPMI Callable Version Slice

## Context and goal
After the descriptor-return increment on `INT 2Fh AX=1687h`, the next roadmap task was to expose the first real callable DPMI slice. The goal of this change is to add a minimal but standards-aligned callable service and validate it with a third M6 smoke binary.

## Files touched
- `boot/proto/services.h`
- `stage2/src/shell.c`
- `stage2/src/stage2.c`
- `com/m6_dpmi_call_smoke/ciuk31.c`
- `com/m6_dpmi_call_smoke/linker.ld`
- `scripts/test_m6_dpmi_call_smoke.sh`
- `scripts/test_m6_pmode_contract.sh`
- `scripts/test_doom_readiness_m6.sh`
- `run_ciukios.sh`
- `Makefile`
- `docs/m6-dos-extender-requirements.md`
- `Roadmap.md`
- `docs/roadmap-ciukios-doom.md`

## Decisions made
1. Added `svc->int31` to the services ABI instead of overloading `int2f`, so callable DPMI growth stays explicit.
2. Implemented only `INT 31h AX=0400h` (Get Version) because it is a standard, always-successful DPMI query and the smallest honest callable increment after descriptor discovery.
3. Returned a DPMI 0.9-style host profile (`AX=005Ah`, 32-bit host flag set, processor type `80486`, PIC bases `08h/70h`) as a conservative regression contract.
4. Added `CIUK31.EXE` as a third shallow smoke after `CIUK4GW.EXE` and `CIUKDPM.EXE`.
5. Updated roadmap wording to show that the next remaining step is now the first real extender-bootstrap behavior, not the first callable DPMI slice in general.

## Validation performed
- `make all`
- `make test-m6-dpmi-call-smoke`
- `make test-m6-pmode`
- `TIMEOUT_SECONDS=1 bash scripts/test_doom_readiness_m6.sh`

## Risks and next step
- The new callable slice is still only a version query; it does not bootstrap a real DPMI client.
- The next M6 task should be to choose the first non-trivial extender regression target and implement only the first bootstrap-facing DPMI behavior that target requires.