# 2026-04-17 - M6 DPMI Descriptor Slice

## Context and goal
The roadmap moved from docs-only milestone planning back into the first real M6 blocker. The immediate goal was to extend the current `INT 2Fh AX=1687h` path beyond pure presence-check smoke and add a stronger regression that validates descriptor metadata before moving on to a callable DPMI service used by a non-trivial extender binary.

## Files touched
- `stage2/src/shell.c`
- `stage2/src/stage2.c`
- `com/m6_dpmi_smoke/ciukdpm.c`
- `com/m6_dpmi_smoke/linker.ld`
- `scripts/test_m6_dpmi_smoke.sh`
- `scripts/test_m6_dos4gw_smoke.sh`
- `scripts/test_m6_pmode_contract.sh`
- `scripts/test_doom_readiness_m6.sh`
- `run_ciukios.sh`
- `Makefile`
- `docs/m6-dos-extender-requirements.md`
- `Roadmap.md`
- `docs/roadmap-ciukios-doom.md`

## Decisions made
1. Kept the existing `CIUK4GW.EXE` smoke unchanged as the shallowest host-query contract.
2. Extended `AX=1687h` to return non-zero descriptor metadata (`SI`, `ES:DI`) while still staying inside a conservative, testable slice.
3. Added `CIUKDPM.EXE` as a second shallow smoke that requires descriptor metadata, but does not pretend to validate a full DPMI entrypoint call yet.
4. Wired the new smoke into the image builder, dedicated gate, PMODE marker coverage, and the aggregate M6 readiness gate.
5. Updated roadmap wording to reflect that the descriptor-return slice is done and the next concrete step is the first callable DPMI service for a real extender target.

## Validation performed
- `make all`
- `make test-m6-dpmi-smoke`
- `make test-m6-dos4gw-smoke`
- `TIMEOUT_SECONDS=1 bash scripts/test_doom_readiness_m6.sh`

## Risks and next step
- `AX=1687h` still returns descriptor metadata only; no real DPMI entrypoint is callable yet.
- The aggregate M6 gate remains partly dependent on static fallback because serial/runtime capture is still flaky on this host.
- The next step should be to freeze the first non-trivial extender regression target and implement only the first callable DPMI service that target actually needs.