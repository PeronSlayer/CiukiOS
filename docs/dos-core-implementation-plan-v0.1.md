# CiukiOS DOS Core Implementation Plan v0.1

## 1. Objective
Deliver the DOS core in deterministic milestones, from BIOS boot to stable DOS runtime, then desktop-enabling surfaces.

## 2. Milestone Plan

### M0 - BIOS Stage0 Boot Baseline
1. Deliver bootable 16-bit boot sector on `floppy` profile.
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
1. FAT12 full baseline on `floppy` profile.
2. Handle-based I/O (`open/read/write/seek/close`) and directory traversal.
3. DOS-style path normalization and error mapping.
4. Gate: DOS file API end-to-end suite.

### M5 - BIOS and Interactive Surface Expansion
1. Expand required `INT 10h/13h/16h/1Ah` behavior.
2. Add stable keyboard/timer contracts and baseline mouse (`INT 33h`).
3. Gate: interactive stability and timing regression tests.

### M6 - GUI-Ready DOS Core Surface
1. Freeze DOS core contracts for desktop/runtime layering.
2. Add compatibility-critical APIs for OpenGEM bring-up path.
3. Gate: OpenGEM pre-desktop boot path reaches deterministic runtime checkpoints.

## 3. Cross-cutting Requirements
1. Every milestone must include automated regression gates.
2. Every major behavior change must update compatibility docs.
3. Logging markers must remain stable and machine-checkable.

## 4. Definition of Done (per Milestone)
1. Build is reproducible.
2. Tests are reproducible and pass.
3. Changelog contains major impact only.
4. Documentation and contracts are updated in concise English.
