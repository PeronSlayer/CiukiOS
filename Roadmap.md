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
**STATUS: tracking on pre-Alpha v0.5.3**
- Focus now: stabilize shell path-command behavior (`move/mv` and rename flow) across floppy (FAT12) and full (FAT16) regression lanes.
- Next sprint: continue graphics/runtime increments while preserving boot determinism and Stage1 command compatibility.

## Phase 3.5 - CiukiOS Installer (Setup project)
> Tracked separately under `setup/`. Prerequisite: stable Phase 3 runtime.
1. DOS-Setup-style text-mode TUI installer binary (`SETUP.COM`).
2. Multi-floppy distribution: N × 1.44MB images with disk-swap engine.
3. CD-ROM distribution: single bootable ISO 9660 image.
4. Installation flow: drive detection, FAT16 format, file copy, config write.
5. Component selection: Minimal / Standard / Full.
- See `setup/README.md` for full TODO breakdown.

## Phase 4 - DOOM Milestone
1. Optimize mode 13h/VGA rendering path.
2. Add the minimum extender compatibility needed by complex DOS binaries.
3. Milestone: DOOM boots and is playable.

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
