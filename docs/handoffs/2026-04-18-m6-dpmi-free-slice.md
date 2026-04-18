# 2026-04-18 - M6 DPMI Free-Memory Slice

## Context and goal
The M6 smoke chain already covered host detect, version query, raw-mode bootstrap, allocate-LDT, and allocate-memory slices. The next small but meaningful step was to make DPMI memory allocation stateful enough that a later free call can validate handle ownership instead of only observing a synthetic success shape.

## Files touched
- `stage2/src/shell.c`
- `stage2/src/stage2.c`
- `Makefile`
- `run_ciukios.sh`
- `scripts/test_doom_readiness_m6.sh`
- `scripts/test_m6_dpmi_free_smoke.sh`
- `com/m6_dpmi_free_smoke/ciukrel.c`
- `com/m6_dpmi_free_smoke/linker.ld`
- `docs/m6-dos-extender-requirements.md`
- `docs/roadmap-ciukios-doom.md`
- `documentation.md`

## Decisions made
1. Added `INT 31h AX=0502h` as the next callable DPMI slice because it builds directly on the existing `AX=0501h` allocate-memory contract and forces stateful bookkeeping instead of a purely synthetic one-shot success.
2. Upgraded the `AX=0501h` implementation from a constant fake return to a minimal stateful allocator over a synthetic DPMI memory window.
3. Added `CIUKREL.EXE` as a new MZ smoke that allocates a block, frees it, then confirms a duplicate free is rejected with an invalid-handle style error.
4. Promoted the new smoke into the aggregate M6 readiness gate so the documentation and validation chain reflect the stronger baseline.

## Validation performed
1. `make all`
2. `make test-m6-dpmi-free-smoke`
3. `make test-m6-dpmi-mem-smoke`

## Risks and next step
1. The DPMI memory model is still synthetic and does not yet expose resize/free-info semantics or real DOS/4GW protected-mode execution.
2. The allocator is intentionally minimal and bounded; it exists to validate stateful allocation/free behavior, not to model the full DPMI spec.
3. Next step: add the first non-trivial DOS-extender regression target that uses the current stateful memory slices to progress beyond shallow bootstrap probes.