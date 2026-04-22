# OPENGEM-044 Stage 3B — trampoline live smoke

## Context and goal
Stage 3B had to exercise the already-landed Task A long↔legacy-PM trampoline for the first time without changing the boot path. The requested surface was a shell-only smoke command `mstest trampoline-smoke` that arms the existing Task A gates, jumps into a dedicated `.code32` body, returns, and leaves a deterministic debugcon marker for optional runtime validation.

## Files touched
- `stage2/include/mode_switch.h`
- `stage2/src/shell.c`
- `stage2/src/mstest_pm32_body.S`
- `scripts/test_mstest_trampoline.sh`
- `Makefile`
- `documentation.md`

## Decisions made
- Re-exported the trampoline-live API and magic in `mode_switch.h` instead of relying on private symbols from `mode_switch.c`.
- Added a dedicated PM32 body `mstest_pm32_body` that writes `OPENGEM-044-RT` to port `0xE9`; this keeps Stage 3B independent from Task B's placeholder body.
- Wired the shell command through a private helper `shell_mstest_trampoline_smoke()` and kept all `mode_switch_*` references behind token-concat macros, preserving the existing Task A leak gate.
- Kept the feature opt-in and shell-triggered only; no boot-path caller was added.

## Validation performed
- `bash scripts/test_mode_switch.sh`
- `bash scripts/test_mstest_trampoline.sh`
- `make build/stage2.elf`

## Risks and next step
- Runtime execution under QEMU/debugcon is still optional and was not exercised automatically here.
- Next step is a user-triggered smoke on the branch with debugcon enabled to confirm the serial marker `OPENGEM-044-RT` appears during the long→legacy→long round-trip.