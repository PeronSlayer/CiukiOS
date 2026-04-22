# Handoff - MZ Loader Hardening + Deterministic Regression

Date: 2026-04-16
Branch baseline: main

## 1. Context and goal
First takeover batch aligned with roadmap priority on EXE/MZ compatibility depth.
Goal:
1. Improve DOS MZ loader semantics for real binaries (declared image bounds vs raw file size).
2. Add deterministic non-QEMU regression tests for MZ parse/relocation behavior.

## 2. Files touched
1. Makefile
2. stage2/src/dos_mz.c
3. stage2/tests/mz_regression.c (new)
4. scripts/test_mz_regression.sh (new)
5. scripts/test_stage2_boot.sh
6. run_ciukios.sh

## 3. Decisions made
1. MZ parse now computes module size from declared EXE length (`total_pages`, `bytes_in_last_page`) instead of full file length.
2. Overlay data after declared image is ignored by loader semantics.
3. Relocation fixups are now validated against declared loadable image bounds, not full physical file buffer.
4. Added host-side deterministic regression suite to validate:
   - baseline parse + relocation patch
   - overlay is not included in module size
   - relocation outside declared module is rejected
5. Added `make test-mz-regression` target invoking script via `bash` (policy-safe, no executable bit dependency).
6. Follow-up (option 1): strengthened stage2 gate orchestration by splitting prebuild from boot timeout in `scripts/test_stage2_boot.sh`.
7. Added `CIUKIOS_SKIP_BUILD` support in `run_ciukios.sh` so tests can prebuild once and run boot checks deterministically.
8. Added optional QEMU runtime knobs in `run_ciukios.sh` for non-interactive environments (`CIUKIOS_QEMU_HEADLESS`, `CIUKIOS_QEMU_BOOT_ORDER`).
9. Added stage2 serial-capture precheck in `scripts/test_stage2_boot.sh`:
   - if QEMU launch is detected but no loader/stage2 serial markers are captured, gate now fails with explicit `[INFRA]` diagnostics
   - includes debugcon presence/absence hint to speed up host troubleshooting

## 4. Validation performed
Executed:
1. `make test-mz-regression` -> PASS
2. `make check-int21-matrix` -> PASS
3. `make test-stage2` -> FAIL (gate timeout path; required serial markers missing while run script is still in build/launch output)
4. `make test-int21` not reached in combined gate command due stage2 failure.
5. Re-ran `make test-stage2` after follow-up changes (`prebuild + skip-build + headless + boot-order`) -> still FAIL in this host: log reaches `[CiukiOS] Starting QEMU...` with no subsequent serial markers.
6. Re-ran `make test-stage2` after serial diagnostic precheck -> FAIL with explicit infra classification:
   - `[INFRA] no loader/stage2 serial markers captured after QEMU launch.`
   - `[INFRA] debugcon log not found: /home/peronslayer/Desktop/CiukiOS/build/debugcon.log`

Notes on failure:
- Failure observed in this environment appears infrastructural/timing-related in `scripts/test_stage2_boot.sh` execution path (long build + launch log before expected markers), not directly tied to MZ-only code changes.
- After prebuild/skip-build split, failure signature changed: build is no longer inside timeout window, but QEMU serial output still does not surface on this host after launch.

## 5. Risks and next step
Risks:
1. Full runtime gates (`test-stage2`, `test-int21`) still require a clean passing run on target runner before merge.
2. MZ runtime dispatch remains MVP for 16-bit binaries; this batch only hardens load semantics and test coverage.
3. Stage2 gate behavior may vary by host QEMU/firmware runtime characteristics; current runner shows zero post-launch serial lines.
4. This runner appears unable to provide expected serial/debugcon stream for the current QEMU invocation; functional regressions cannot be inferred from this gate result alone.

Next step:
1. Re-run stage2/int21 gates on a runner with stable QEMU log timing and confirm green.
2. Extend deterministic MZ suite with additional edge cases (invalid page math, relocation table overlap, SS:SP boundary conditions).
3. If needed, add a dedicated QEMU serial diagnostics precheck to fail fast when host does not expose boot serial output.
