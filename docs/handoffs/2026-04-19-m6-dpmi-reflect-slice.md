# 2026-04-19 - M6 DPMI Reflect Slice

## Context and goal
The M6 smoke chain already covered host detect, version query, raw-mode bootstrap, allocate-LDT, allocate-memory, and free-memory slices. The next meaningful step was to add the first minimal interrupt-reflection callable slice so a DOS extender path can reflect a real-mode-style interrupt frame through the existing DOS runtime instead of only probing metadata.

## Files touched
- `stage2/src/shell.c`
- `stage2/src/stage2.c`
- `Makefile`
- `run_ciukios.sh`
- `scripts/test_doom_readiness_m6.sh`
- `scripts/test_m6_dpmi_reflect_smoke.sh`
- `com/m6_dpmi_reflect_smoke/ciukrmi.c`
- `com/m6_dpmi_reflect_smoke/linker.ld`
- `docs/m6-dos-extender-requirements.md`
- `docs/roadmap-ciukios-doom.md`

## Decisions made
1. Added `INT 31h AX=0300h` as a minimal callable DPMI reflect slice with a deliberately narrow contract: only `BL=0x21` is supported and the reflected register frame must live inside the active DOS image at `ES:DI`.
2. Reused the existing DOS runtime path by routing the reflected frame into `shell_com_int21(ctx, rm_regs)` instead of inventing a second INT 21h execution path.
3. Added `CIUKRMI.EXE` as a new MZ smoke that queries host presence/version, reflects `INT 21h AH=30h`, and exits with `0x59` only if the DOS-version frame comes back as expected.
4. Wired the new smoke into the image build and the aggregate M6 readiness gate so future regressions catch missing packaging or missing host wiring, not just code-level omissions.

## Validation performed
1. `make all`
2. `TIMEOUT_SECONDS=1 make test-m6-dpmi-reflect-smoke`

## Risks and next step
1. On this host, QEMU headless serial capture still often fails to provide runtime markers, so the validated result for this task is currently the static fallback path rather than a runtime green path.
2. `AX=0300h` is intentionally narrow and only supports the `INT 21h` reflection needed for the current regression target; broader interrupt reflection remains future work.
3. Next step: expand from synthetic interrupt reflection toward the next DOS-extender path that consumes reflected DOS services in a more realistic protected-mode flow.