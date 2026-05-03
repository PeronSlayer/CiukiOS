# SETUP.COM Text-Mode Installer Checklist

Use this checklist to track implementation and acceptance evidence for the Phase 4 installer execution track.

## Phase 3.5 closure baseline (completed)
- [x] Setup stream boundaries and architecture baseline documented in `setup/README.md`.
- [x] Installer phased plan baseline documented in `setup/README.md`.
- [x] Setup execution checklist baseline documented in `setup/SETUP_COM_MVP_CHECKLIST.md`.
- [x] Setup helper scaffolding available in `scripts/setup_prepare_artifacts.sh`.
- [x] Historical trace preserved: 2026-04-30 closure recorded as a FOUNDATION/PLACEHOLDER baseline.
- [x] Functional closure upgrade recorded on 2026-05-01 for a FULL-only MVP baseline.

## Stream C FULL-only packaging status (2026-05-01)
- [x] Full image build compiles `src/com/setup.asm` and packages `APPS/SETUP.COM` in FAT16 map.
- [x] Full-only installer acceptance script added: `scripts/qemu_test_setup_full_acceptance.sh`.
- [x] Packaging acceptance evidence captured with marker checks (directory entry, FAT chain, payload bytes).
- [x] Full stage1 selftest gate green.
	- Evidence (2026-05-01): `./scripts/qemu_test_full_stage1.sh` PASS.
- [x] Full-only installer acceptance gate green.
	- Evidence (2026-05-01): `./scripts/qemu_test_setup_full_acceptance.sh` PASS (smoke boot + marker checks).
- [x] Phase 3.5 functional MVP closure formalized as FULL-only baseline.
	- Scope caveat: multi-floppy and extended CD installer workflows remain post-MVP backlog items.

## Phase 4 installer execution backlog (CLOSED 2026-05-03)

## A. Execution contracts hardening
- [x] Freeze installer input manifest fields (source media, profile, target path) for executable implementation.
- [x] Freeze installer output report fields (status, error code, copied files) for deterministic diagnostics.
- [x] Record critical-path dependencies between UX, workflow engine, and media contracts with owner/date.
- Evidence (2026-05-03): `src/com/setup.asm` report contract fields (`REPORT_SCHEMA`, `INPUT_MEDIA`, `INPUT_TARGET`, `INPUT_PROFILE`, `STEP_HEX`, `RETRY_COUNT_HEX`, `KB_KEYS_HEX`, `KB_NAV_HEX`, `FILES_*`, `FAIL_CODE_HEX`) + `scripts/qemu_test_setup_installer_scenarios.sh` marker `REPORT_CONTRACT_BIN=PASS`.
- Dependency register (2026-05-03): UX + workflow + media contracts tracked in setup stream execution owner (`setup stream owner`), implementation owner (`ciukios-implementation-worker`), verification owner (`QA setup acceptance`) with evidence bundle in `build/full/phase4_setup_*.{log,rc}`.

## B. Text-mode UX flow
- [x] Create welcome screen with explicit key hints.
- [x] Create component selection screen (Minimal/Standard/Full).
- [x] Create target-drive selection and confirmation screen.
- Evidence (2026-04-30): `setup/SETUP_COM_B1_B2_B3_SCREEN_FLOW.md`, `setup/SETUP_COM_B1_B2_B3_PROMPT_COPY.md`.
- Test status (2026-04-30): `scripts/setup_prepare_artifacts.sh --validate-only` PASS; `scripts/setup_prepare_artifacts.sh --dry-run` PASS.
- [x] Create progress screen with current file/media context.
- [x] Create completion and failure screens with actionable next steps.
- [x] Define keyboard navigation baseline (Up/Down, Enter, Esc, retry path) in setup documents.
- [x] Validate full keyboard navigation at runtime (Up/Down, Enter, Esc, retry path).
- Baseline evidence (2026-04-30): `setup/SETUP_COM_B4_B6_SCREEN_FLOW.md`, `setup/SETUP_COM_B4_B6_PROMPT_COPY.md`, `setup/SETUP_COM_B6_KEYBOARD_VALIDATION.md`.
- Artifact check status (2026-04-30): `scripts/setup_prepare_artifacts.sh --validate-only` PASS; `scripts/setup_prepare_artifacts.sh --dry-run` PASS (document checks only).
- Runtime evidence (2026-05-03): `scripts/qemu_test_setup_installer_scenarios.sh` PASS (`success_minimal` sends Down/Up/Enter and completes; `failure_missing_media` validates retry + Esc cancel).

## C. Installer workflow engine
- [x] Implement deterministic state machine for all screens/steps.
- [x] Add guard checks before advancing each step.
- [x] Add retry/back/cancel handling with safe rollback points.
- [x] Emit structured status and failure codes per step.
- [x] Add timeout-safe prompt handling for media swap requests.
- Evidence (2026-05-03): `src/com/setup.asm` implements timeout-safe key waits (`wait_key_timeout`) and media-swap timeout failure (`0x0603`); `scripts/qemu_test_setup_installer_scenarios.sh` validates both `success_media_swap` and `failure_media_swap_timeout` paths.

## D. Target disk and filesystem (MVP)
- [x] Detect available install targets and filter invalid destinations.
- [x] Implement FAT16 preflight checks.
- [x] Implement FAT16 preparation/format step.
- [x] Verify post-format filesystem sanity before copy.
- [x] Block copy step when preflight/post-format checks fail.
- Evidence (2026-05-03): `src/com/setup.asm` target scan/filter and guard (`detect_targets`, `guard_target_selection`) with explicit invalid-target fail (`0x0203`), preflight gate (`0x0201/0x0202`), FAT16 prepare marker (`FORMAT_OK`) and post-format sanity marker (`SANITY_OK`); runtime evidence from `scripts/qemu_test_setup_installer_scenarios.sh` markers `failure_invalid_target_*`, `failure_insufficient_space_*`, and success-path completion.

## E. Payload copy and config generation
- [x] Parse source payload manifest from selected media.
- [x] Copy ordered payload set with progress updates.
- [x] Verify copied file count and byte totals.
- [x] Support media-swap prompt/retry for multi-disk paths.
- [x] Generate selected-profile config artifacts.
- [x] Persist install report for diagnostics.
- Evidence (2026-05-03): `scripts/qemu_test_setup_installer_scenarios.sh` validates media-swap success + timeout (`success_media_swap_*`, `failure_media_swap_timeout_*`), ordered copy/report counters (`FILES_*`, `BYTES_*`), config output (`build/full/setup_scenario_success_minimal.cfg.txt`, `build/full/setup_scenario_success_media_swap.cfg.txt`), and deterministic reports (`build/full/setup_scenario_success_minimal.report.txt`, `build/full/setup_scenario_success_media_swap.report.txt`, `build/full/setup_scenario_failure_missing_media.report.txt`).
- Evidence (2026-05-03, manifest media source fix): `build/full/setup_scenario_success_minimal.report.txt` and `build/full/setup_scenario_success_media_swap.report.txt` both record `MANIFEST_MEDIA_HEX=0001`; `build/full/setup_scenario_manifest_invalid_header_fallback.report.txt` records `MANIFEST_MEDIA_HEX=0000` with fallback marker in `build/full/setup_scenario_manifest_invalid_header_fallback.serial.log` (`Manifest fallback: invalid header.`).

## F. QA and acceptance evidence
- [x] Run dry install simulation (no target writes) and archive logs.
- [x] Run success case for Minimal profile and archive logs.
- [x] Run failure case: bad/removed media and validate retry/abort behavior.
- [x] Run failure case: insufficient target space and validate user feedback.
- [x] Capture final evidence bundle (logs, manifests, report).
- Evidence (2026-05-03): required command suite green in this cycle: `./scripts/build_full.sh` RC=0, `./scripts/qemu_test_setup_full_acceptance.sh` RC=0, `./scripts/qemu_test_setup_installer_scenarios.sh` RC=0; scenario artifacts include `build/full/setup_scenario_success_minimal.*`, `build/full/setup_scenario_success_media_swap.*`, `build/full/setup_scenario_failure_media_swap_timeout.*`, `build/full/setup_scenario_failure_missing_media.*`, `build/full/setup_scenario_dry_run_minimal.*`, `build/full/setup_scenario_failure_insufficient_space.*`.
- Artifact snapshots (2026-05-03): `build/full/phase4_setup_success_mdir_ciukios.txt`, `build/full/phase4_setup_failure_mdir_ciukios.txt`.

## Phase 4 acceptance gate
Mark the installer execution backlog complete only when all conditions are true:
- [x] Sections A-F are fully checked.
- [x] No blocking defects in critical path (drive select -> format -> copy -> finalize).
- [x] Installer run produces deterministic report artifacts.
- [x] Installer closure evidence remains reproducible independently from separate runtime-core work tracked in the same branch.
