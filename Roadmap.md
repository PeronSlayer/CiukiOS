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
**STATUS: CLOSED - FOUNDATION/PLACEHOLDER (2026-04-30)**
- Closure scope accepted: setup planning and scaffolding artifacts already present in repository.
- Evidence: `setup/README.md`, `setup/SETUP_COM_MVP_CHECKLIST.md`, and `scripts/setup_prepare_artifacts.sh`.
- Explicit caveat: this closure does not claim end-to-end executable installer completion.
- Transition: full installer execution work moves into the active Phase 4 installer execution track.
- See `setup/README.md` for the active execution backlog.

## Phase 4 - DOOM Milestone + Installer Execution Track
**STATUS: ACTIVE (from 2026-04-30)**
1. Execute installer backlog moved from Phase 3.5 (workflow engine, media swap flow, FAT16 prep, payload copy, config/reporting).
2. Produce reproducible installer validation evidence for success/failure scenarios.
3. Optimize mode 13h/VGA rendering path.
4. Add the minimum extender compatibility needed by complex DOS binaries.
5. Milestone: DOOM boots and is playable.

## Phase 5 - Windows pre-NT Milestones
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
