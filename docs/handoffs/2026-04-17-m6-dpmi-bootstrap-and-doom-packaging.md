# 2026-04-17 - M6 DPMI Bootstrap Slice and DOOM Packaging Baseline

## Context and goal
After the callable DPMI version slice (`INT 31h AX=0400h`), the next roadmap step was to expose the first bootstrap-facing DPMI behavior and wire the first deterministic DOOM packaging/discovery path. The goal of this change is to add a fourth shallow M6 smoke for `AX=0306h`, make the DOOM image layout reproducible, and align the aggregate gates/docs with the new baseline.

## Files touched
- `boot/proto/services.h`
- `stage2/src/shell.c`
- `stage2/src/stage2.c`
- `com/m6_dpmi_bootstrap_smoke/ciuk306.c`
- `com/m6_dpmi_bootstrap_smoke/linker.ld`
- `scripts/test_m6_dpmi_bootstrap_smoke.sh`
- `scripts/test_doom_target_packaging.sh`
- `scripts/test_m6_pmode_contract.sh`
- `scripts/test_doom_readiness_m6.sh`
- `run_ciukios.sh`
- `Makefile`
- `docs/m6-dos-extender-requirements.md`
- `docs/roadmap-ciukios-doom.md`
- `Roadmap.md`

## Decisions made
1. Implemented only `INT 31h AX=0306h` as the next DPMI increment because it is a bootstrap-facing query that keeps scope tight while moving beyond pure host presence/version checks.
2. Added `CIUK306.EXE` as the fourth shallow smoke after `CIUKPM.EXE`, `CIUK4GW.EXE`, `CIUKDPM.EXE`, and `CIUK31.EXE`.
3. Added an explicit stage2 marker for the new bootstrap slice so the contract gate can validate it in both runtime and static-fallback modes.
4. Extended `run_ciukios.sh` with deterministic, user-supplied DOOM asset packaging (`DOOM.EXE`, `DOOM1.WAD`, optional `DEFAULT.CFG`, generated `DOOM.BAT`) under `/EFI/CiukiOS`.
5. Fixed the packaging harness to verify files by extracting them from the FAT image instead of grepping `mdir` output, because FAT 8.3 directory listings split name and extension.
6. Promoted the new bootstrap smoke and the packaging harness into the aggregate M6 readiness gate so the roadmap/docs reflect the actual baseline.

## Validation performed
- `make test-m6-dpmi-bootstrap-smoke`
- `make test-doom-target-packaging`
- `make test-m6-pmode`

## Risks and next step
- The current baseline still stops at shallow DPMI/bootstrap probes; it does not execute a real DOS/4GW-class workload yet.
- The next M6 task should be to replace `CIUK306.EXE` as the ceiling with a non-trivial extender regression target that exercises actual bootstrap/execution flow.
- The next DOOM task should be to extend the packaging baseline into a staged boot-to-game harness with failure taxonomy (`binary`, `WAD`, `extender`, `video`, `menu`).