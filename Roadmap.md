# CiukiOS Legacy v2 Roadmap

## Vision
Build a simple, native x86 BIOS operating system that runs DOS and pre-NT workloads without CPU emulation in the final runtime path.

## Normative References
1. DOS core contract: `docs/dos-core-spec-v0.1.md`
2. DOS core execution plan: `docs/dos-core-implementation-plan-v0.1.md`

## Phase 0 - Reset Foundation
1. Archive previous project state under `OLD/`.
2. Define and freeze legacy-first architecture.
3. Establish new AI and development operating rules.

## Phase 1 - Minimal Legacy Boot (Floppy-first)
1. 16-bit boot sector (512B) and multi-stage loader.
2. Real-mode initialization and core BIOS services (`INT 10h/13h/16h/1Ah`).
3. Minimal x86 kernel and basic shell.
4. `floppy` profile constrained to 1.44MB.

## Phase 2 - Native DOS Runtime
1. Native `.COM/.EXE` loader.
2. PSP/MCB and conventional memory management (with future UMB/HMA extensions).
3. High-compatibility `INT 21h` surface.
4. FAT12/FAT16 baseline for floppy profile.

## Phase 3 - DOS Graphics Runtime (Shell-first)
1. Native VGA/VBE path.
2. Extended `INT 10h` plus robust timer/mouse/input services.
3. Incremental graphics services that keep shell stability as primary target.
4. Milestone: stable graphics/runtime services on real hardware with shell loop preserved.
**STATUS: CLOSED (2026-04-30)**
- Evidence: released `CiukiOS pre-Alpha v0.5.3` after shell `move/mv` runtime stabilization.
- Evidence: fixed `INT 21h AH=56h` rename/move behavior with deterministic semantics.
- Evidence: reran floppy (FAT12) and full (FAT16) regression lanes with stable outcomes.

## Phase 3.5 - CiukiOS Installer (Setup project)
> Tracked separately under `setup/`. Prerequisite: stable Phase 3 runtime.
1. DOS-Setup-style text-mode TUI installer binary (`SETUP.COM`).
2. Multi-floppy distribution: N × 1.44MB images with disk-swap engine.
3. CD-ROM distribution: single bootable ISO 9660 image.
4. Installation flow: drive detection, FAT16 format, file copy, config write.
5. Component selection: Minimal / Standard / Full.
**STATUS: CLOSED - FUNCTIONAL MVP (FULL-only) (update 2026-05-01)**
- Historical trace: Phase 3.5 was first closed as a FOUNDATION/PLACEHOLDER baseline on 2026-04-30.
- Functional closure update (2026-05-01): MVP installer baseline is executable on the full profile, with `SETUP.COM` packaged in the FAT16 full image.
- Evidence (2026-05-01): `scripts/qemu_test_full_stage1.sh` PASS.
- Evidence (2026-05-01): `scripts/qemu_test_setup_full_acceptance.sh` PASS.
- Scope caveat: closure is full-profile only; floppy lane is not a required installer baseline.
- Advanced backlog note: multi-floppy distribution and extended CD installer workflow remain post-MVP backlog items for a later phase.
- See `setup/README.md` for post-MVP installer maintenance and advanced backlog tracking.

## Phase 4 - DOOM Milestone + Installer Execution Track
**STATUS: CLOSED - DOOM GAMEPLAY PLAYABLE (2026-05-04)**
**INSTALLER EXECUTION LANE: CLOSED (2026-05-03)**
**RUNTIME/DOOM LANE: CLOSED - PLAYABLE (2026-05-04)**
1. Installer execution backlog (post-MVP hardening, media-swap flows, and failure-path validation) is completed.
2. Reproducible installer evidence bundle is completed and archived.
3. Mode 13h/VGA and DOS extender compatibility reached the level required for DOOM gameplay on the full FAT16 profile.
4. Minimum extender compatibility for complex DOS binaries is proven through DOS/4GW startup, WAD loading, refresh/playloop/input/sound/HUD/status-bar initialization, and rendered gameplay.
5. Milestone: DOOM boots and is playable. **COMPLETED (2026-05-04)**
- Evidence (2026-05-01): released `CiukiOS pre-Alpha v0.5.4` after shell input stability improvements (hold-key repeat, wrap, backspace) and FAT16 footer telemetry stabilization (`CPU/DSK/RAM`).
- Evidence (2026-05-01): reran cross-profile build/regression lanes for floppy (FAT12) and full (FAT16) with stable high-level outcomes.
- Evidence (2026-05-03): `./scripts/build_full.sh` PASS.
- Evidence (2026-05-03): `./scripts/qemu_test_setup_full_acceptance.sh` PASS.
- Evidence (2026-05-03): `./scripts/qemu_test_setup_installer_scenarios.sh` PASS.
- Evidence (2026-05-04): `make build-full` PASS.
- Evidence (2026-05-04): `make qemu-test-full` PASS.
- Evidence (2026-05-04): `DO_BUILD=0 DOOM_TAXONOMY_DISPLAY_MODE=none DOOM_TAXONOMY_SCREENSHOT=build/full/doom_post_status_after_rebuild.ppm QEMU_TIMEOUT_SEC=120 DOOM_TAXONOMY_OBSERVE_SEC=45 DOOM_TAXONOMY_MIN_STAGE=video_init make qemu-test-full-doom-taxonomy` PASS.
- Evidence (2026-05-04): visual screenshot `build/full/doom_post_status_after_rebuild.png` shows DOOM gameplay viewport and HUD after status-bar initialization.
- Evidence (2026-05-04): project owner manually confirmed DOOM is playable interactively on the generated full-profile image.
- Release: `CiukiOS pre-Alpha v0.6.1`.
- Scope note: follow-up audio/driver polish and richer gameplay taxonomy remain hardening work, not Phase 4 closure blockers.

## Phase 5 - Windows pre-NT Milestones
**STATUS: ACTIVE (next major compatibility direction after v0.6.3)**
1. Expand DOS compatibility required by Windows 3.x/95/98 bootstrap and runtime paths.
2. Extend protected-mode services and interrupt/timer compatibility.
3. Add required device and setup-path behavior.
4. Milestones: Windows 3.x -> Windows 95 -> Windows 98.

## Phase 6 - Build and Release Discipline
1. `floppy` profile: minimal, portable, diagnostics-first.
2. `full` profile: complete runtime with shell-first behavior.
3. Regression pipeline on emulators and real legacy hardware.

## Advancement Criteria
1. Every milestone must have reproducible tests.
2. No merge to `main` without explicit user approval.
3. No CPU-emulation shortcuts as final runtime solution.
