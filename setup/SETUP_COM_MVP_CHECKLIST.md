# SETUP.COM Text-Mode Installer MVP Checklist

Use this checklist to track implementation and acceptance evidence for Phase 3.5.

## A. Bootstrap and contracts
- [ ] Confirm setup stream boundaries (no runtime core edits) are documented.
- [ ] Freeze MVP scope for `SETUP.COM` text-mode flow.
- [ ] Define installer input manifest fields (source media, profile, target path).
- [ ] Define installer output report fields (status, error code, copied files).
- [ ] Record critical-path dependencies between UX, workflow engine, and media contracts.

## B. Text-mode UX flow
- [ ] Create welcome screen with explicit key hints.
- [ ] Create component selection screen (Minimal/Standard/Full).
- [ ] Create target-drive selection and confirmation screen.
- [ ] Create progress screen with current file/media context.
- [ ] Create completion and failure screens with actionable next steps.
- [ ] Validate full keyboard navigation (Up/Down, Enter, Esc, retry path).

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

## MVP acceptance gate
Mark MVP complete only when all conditions are true:
- [ ] Sections A-F are fully checked.
- [ ] No blocking defects in critical path (drive select -> format -> copy -> finalize).
- [ ] Installer run produces deterministic report artifacts.
- [ ] Setup stream remains isolated from runtime core files.
