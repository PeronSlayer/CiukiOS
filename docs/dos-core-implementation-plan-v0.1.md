# CiukiOS DOS Core Implementation Plan v0.1

## 1. Objective
Deliver the DOS core in deterministic milestones, from BIOS boot to stable DOS runtime, then desktop-enabling surfaces.

## Current Interpretation

This file preserves the original milestone decomposition. It is not the current phase-status authority. DOS-core ownership has completed its wholesale move out of Stage1; current execution status is governed by `../Roadmap.md`, `stage1-runtime-split-plan-v0.1.md`, `dos-compatibility-matrix-v0.1.md`, and `current-milestones.md`.

The boot, COM/MZ, file/path, graphics, input, and mouse baselines below have implementation evidence and their normal runtime owners are in CIUKIDOS. The active work is to deepen behavioral compatibility and prove those contracts against the quantitative Phase 6 corpus without reopening the completed Phase 5 placement boundary.

## 2. Milestone Plan

### M0 - BIOS Stage0 Boot Baseline
1. Deliver a bootable 16-bit BIOS boot baseline; the original milestone used the `floppy` profile.
2. Emit deterministic boot marker on screen and serial.
3. Gate: QEMU floppy boot marker detection.

### M1 - Stage1 Loader and Disk Read Path
1. Add stage1 loader read path from floppy sectors.
2. Load and transfer control to a structured kernel entry.
3. Gate: deterministic loader marker sequence and handoff verification.

### M2 - Minimal DOS Kernel Skeleton
1. Define DOS kernel entry contract and runtime state.
2. Add baseline interrupt vector initialization.
3. Implement first core `INT 21h` subset (process exit/status and basic console).
4. Gate: small DOS runtime smoke program execution.

### M3 - Program Loader Baseline (.COM then .EXE)
1. `.COM` loader with PSP baseline behavior.
2. `.EXE` MZ loader with relocation support.
3. Core memory allocation lifecycle through DOS semantics.
4. Gate: deterministic COM and EXE compatibility tests.

### M4 - DOS File and Path Compatibility
1. FAT12 historical baseline on `floppy`; FAT16 is the active full/full-CD baseline.
2. Handle-based I/O (`open/read/write/seek/close`) and directory traversal.
3. DOS-style path normalization and error mapping.
4. Preserve 32-bit FAT16 LBAs across reads, writes, directory mutations, EDD, and CHS fallback.
5. Gate: DOS file API end-to-end suite plus a deterministic full-CD read beyond LBA 65,535.

### M5 - BIOS and Interactive Surface Expansion
1. Expand required `INT 10h/13h/16h/1Ah` behavior.
2. Add stable keyboard/timer contracts and baseline mouse (`INT 33h`).
3. Gate: interactive stability and timing regression tests.

### M6 - GUI-Ready DOS Core Surface
1. Freeze DOS core contracts for desktop/runtime layering.
2. Add compatibility-critical APIs for desktop runtime bring-up path.
3. Gate: desktop pre-desktop boot path reaches deterministic runtime checkpoints.

## 3. Cross-cutting Requirements
1. Every milestone must include automated regression gates.
2. Every major behavior change must update compatibility docs.
3. Logging markers must remain stable and machine-checkable.

## 4. Definition of Done (per Milestone)
1. Build is reproducible.
2. Tests are reproducible and pass.
3. Changelog contains major impact only.
4. Documentation and contracts are updated in concise English.
