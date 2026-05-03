# CiukiOS Setup Stream (Phase 3.5 Functional Closure + Phase 4 Installer Closure + Post-MVP Backlog)

## Status update (2026-04-30)
- Phase 3.5 foundation stream is formally closed as a FOUNDATION/PLACEHOLDER baseline.
- Closure scope accepted: setup planning artifacts and helper scaffolding already present in repository.
- Remaining executable installer implementation and validation tasks are moved to the active Phase 4 installer execution track.

## Status update (2026-05-01)
- Historical trace preserved: the 2026-04-30 placeholder closure remains part of project history.
- Phase 3.5 closure is now upgraded to FUNCTIONAL MVP on a FULL-only baseline.
- Stream C FULL-only packaging baseline implemented: `SETUP.COM` is built from `src/com/setup.asm` and embedded as `APPS/SETUP.COM` in the full FAT16 image.
- Evidence (2026-05-01): `scripts/qemu_test_full_stage1.sh` PASS.
- Evidence (2026-05-01): `scripts/qemu_test_setup_full_acceptance.sh` PASS (smoke boot + installer payload marker checks).
- Scope caveat: full-only baseline remains the installer requirement; floppy lane is not required for this closure.
- Advanced backlog note: multi-floppy media flow and extended CD installer workflow remain post-MVP items for a later phase.

## Status update (2026-05-03)
- Phase 4 installer execution backlog (sections A-F in `setup/SETUP_COM_MVP_CHECKLIST.md`) is closed.
- Deterministic setup reports and scenario evidence are archived for success and failure paths.
- Evidence (2026-05-03): `./scripts/build_full.sh` PASS.
- Evidence (2026-05-03): `./scripts/qemu_test_setup_full_acceptance.sh` PASS.
- Evidence (2026-05-03): `./scripts/qemu_test_setup_installer_scenarios.sh` PASS.
- Scope note: runtime/DOOM milestones stay active under Phase 4 in the main project roadmap.

## Current objective
Keep the closed installer lane reproducible and maintain deterministic setup evidence while preparing only post-MVP extensions.

The stream now focuses on maintenance, regression confidence, and planning for advanced media targets beyond the closed MVP baseline.

## Scope and boundaries
- In scope: installer planning, task tracking, setup-specific helper tooling, setup artifact validation.
- Out of scope: unrelated runtime core architecture changes in src/boot/* unless explicitly approved as cross-stream scope.
- Integration rule: setup can adjust installer-facing packaging/test lanes, but does not redefine DOS runtime contracts without explicit project decision.

## Installer architecture (MVP)
1. `SETUP.COM` TUI frontend:
- Text-mode, keyboard-only navigation.
- Wizard flow with explicit back/next/cancel states.

2. Install orchestrator:
- State machine for workflow progression and restart-safe transitions.
- Consistent error codes for recoverable vs blocking failures.

3. Media abstraction:
- Source providers for multi-floppy sets and single-image CD distribution.
- Unified file-copy request interface consumed by orchestrator.

4. Target disk services:
- Drive detection and target eligibility checks.
- FAT16 preparation path and post-format validation.

5. Payload and config writer:
- Ordered copy queue with integrity checks.
- Config generation for selected profile (Minimal/Standard/Full).

6. Recovery and logging:
- Human-readable failure prompts, disk-swap prompts, and retry controls.
- Install report artifact for post-run diagnostics.

## Step-by-step phased plan (historical execution snapshot)
Critical path execution was Phase B -> Phase C -> Phase D. All phases below are closed for the installer MVP lane.

### Phase A - Bootstrap baseline (completed)
- Status: completed and accepted in Phase 3.5 closure scope (2026-04-30).
- Update (2026-05-01): upgraded to functional FULL-only MVP closure with green validation gates.
- Evidence artifacts:
  - `setup/README.md` (architecture + plan + acceptance criteria).
  - `setup/SETUP_COM_MVP_CHECKLIST.md` (execution checklist baseline).
  - `scripts/setup_prepare_artifacts.sh` (validation and artifact prep helper).
- Verification output: helper script supports dry-run and validate-only setup checks.

### Phase B - Installer shell skeleton (completed)
Stream 1 (UX flow):
- Screen map, key bindings, and navigation behavior.
- Verifiable output: screen-flow document and prompt copy set.

Stream 2 (workflow engine):
- Step state machine and failure/retry transitions.
- Verifiable output: transition table and scenario matrix.

Stream 3 (media contracts):
- Manifest format and media swap protocol.
- Verifiable output: source manifest schema and parser test vectors.

### Phase C - Integration MVP (completed)
- Implement FAT16 target prep checks, file-copy orchestration, and config write.
- Dependency: completion of all Phase B streams.
- Verifiable output: successful end-to-end dry install simulation with deterministic logs.

### Phase D - Hardening and release gate (completed 2026-05-03)
- Regression scenarios for user abort, bad media, and low-space paths.
- Post-MVP packaging rules for advanced media targets (multi-floppy set and extended CD payload manifests).
- Verifiable output: signed-off checklist and reproducible validation logs.
- Closure evidence (2026-05-03): `./scripts/build_full.sh` PASS.
- Closure evidence (2026-05-03): `./scripts/qemu_test_setup_full_acceptance.sh` PASS.
- Closure evidence (2026-05-03): `./scripts/qemu_test_setup_installer_scenarios.sh` PASS.

## Assignments
- Setup stream owner: maintains architecture and acceptance gates.
- Runtime liaison: validates installer/runtime contract compatibility.
- QA owner: runs scenario matrix and archives verification artifacts.

## Risks and mitigations
1. Risk: runtime/setup contract drift.
- Mitigation: keep an explicit installer input manifest and lock it per milestone.

2. Risk: media-swap errors causing partial installs.
- Mitigation: per-step checksums and mandatory retry/abort controls.

3. Risk: FAT16 preparation differences across targets.
- Mitigation: preflight checks plus post-format sanity verification before copy.

## Completion criteria (done)
1. `SETUP.COM` MVP flow is fully keyboard-driven in text mode.
2. Installer can complete Minimal profile path with deterministic logs.
3. FAT16 target preparation and validation pass before payload copy.
4. Copy engine supports media prompt/retry semantics.
5. Config artifacts are generated for the selected profile.
6. Checklist in `setup/SETUP_COM_MVP_CHECKLIST.md` is fully checked with evidence.

## Immediate next action
Keep installer closure green while planning post-MVP advanced media scope (multi-floppy and extended CD workflow).

Recommended maintenance verification:

```bash
./scripts/build_full.sh
./scripts/qemu_test_setup_full_acceptance.sh
./scripts/qemu_test_setup_installer_scenarios.sh
```

If all gates remain green, continue with a separate planning cycle for advanced media distribution work.

## Helper script usage
The setup bootstrap helper validates required setup files and can prepare metadata artifacts under `build/setup/`.

```bash
scripts/setup_prepare_artifacts.sh --help
scripts/setup_prepare_artifacts.sh --dry-run
scripts/setup_prepare_artifacts.sh --validate-only
scripts/setup_prepare_artifacts.sh --output-dir build/setup-bootstrap
```
