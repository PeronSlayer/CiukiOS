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

## Phase 4 installer execution backlog

## A. Execution contracts hardening
- [ ] Freeze installer input manifest fields (source media, profile, target path) for executable implementation.
- [ ] Freeze installer output report fields (status, error code, copied files) for deterministic diagnostics.
- [ ] Record critical-path dependencies between UX, workflow engine, and media contracts with owner/date.

## B. Text-mode UX flow
- [x] Create welcome screen with explicit key hints.
- [x] Create component selection screen (Minimal/Standard/Full).
- [x] Create target-drive selection and confirmation screen.
- Evidence (2026-04-30): `setup/SETUP_COM_B1_B2_B3_SCREEN_FLOW.md`, `setup/SETUP_COM_B1_B2_B3_PROMPT_COPY.md`.
- Test status (2026-04-30): `scripts/setup_prepare_artifacts.sh --validate-only` PASS; `scripts/setup_prepare_artifacts.sh --dry-run` PASS.
- [x] Create progress screen with current file/media context.
- [x] Create completion and failure screens with actionable next steps.
- [x] Define keyboard navigation baseline (Up/Down, Enter, Esc, retry path) in setup documents.
- [ ] Validate full keyboard navigation at runtime (Up/Down, Enter, Esc, retry path).
- Baseline evidence (2026-04-30): `setup/SETUP_COM_B4_B6_SCREEN_FLOW.md`, `setup/SETUP_COM_B4_B6_PROMPT_COPY.md`, `setup/SETUP_COM_B6_KEYBOARD_VALIDATION.md`.
- Artifact check status (2026-04-30): `scripts/setup_prepare_artifacts.sh --validate-only` PASS; `scripts/setup_prepare_artifacts.sh --dry-run` PASS (document checks only).

## C. Installer workflow engine
- [ ] Implement deterministic state machine for all screens/steps.
- [ ] Add guard checks before advancing each step.
- [ ] Add retry/back/cancel handling with safe rollback points.
- [ ] Emit structured status and failure codes per step.
- [ ] Add timeout-safe prompt handling for media swap requests.

## D. Target disk and filesystem (MVP)
- [ ] Detect available install targets and filter invalid destinations.
- [ ] Implement FAT16 preflight checks.
- [ ] Implement FAT16 preparation/format step.
- [ ] Verify post-format filesystem sanity before copy.
- [ ] Block copy step when preflight/post-format checks fail.

## E. Payload copy and config generation
- [ ] Parse source payload manifest from selected media.
- [ ] Copy ordered payload set with progress updates.
- [ ] Verify copied file count and byte totals.
- [ ] Support media-swap prompt/retry for multi-disk paths.
- [ ] Generate selected-profile config artifacts.
- [ ] Persist install report for diagnostics.

## F. QA and acceptance evidence
- [ ] Run dry install simulation (no target writes) and archive logs.
- [ ] Run success case for Minimal profile and archive logs.
- [ ] Run failure case: bad/removed media and validate retry/abort behavior.
- [ ] Run failure case: insufficient target space and validate user feedback.
- [ ] Capture final evidence bundle (logs, manifests, report).

## Phase 4 acceptance gate
Mark the installer execution backlog complete only when all conditions are true:
- [ ] Sections A-F are fully checked.
- [ ] No blocking defects in critical path (drive select -> format -> copy -> finalize).
- [ ] Installer run produces deterministic report artifacts.
- [ ] Setup stream remains isolated from runtime core files.
