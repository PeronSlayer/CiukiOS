# CiukiOS DOS Core Specification v0.1

## 1. Purpose
Define the mandatory behavior of the CiukiOS DOS-compatible core.
This document is normative for the active `full` and `full-cd` runtime baselines. The `floppy` profile retains a legacy/minimal FAT12 subset unless explicitly reopened.

## 2. Strategic Goal
Build a DOS-first operating system that is behaviorally compatible with core MS-DOS / IBM PC DOS semantics, while allowing additive modern features that do not break legacy behavior.

## 3. Compatibility Scope
1. Primary baseline: DOS application compatibility for real-mode software.
2. Behavioral target: practical compatibility with common MS-DOS 5/6 and PC DOS style runtime expectations.
3. Execution target: legacy BIOS x86 systems.

## 4. Architecture Contract
1. BIOS boot remains mandatory.
2. DOS core services are the primary OS interface.
3. GUI/desktop layers are optional and must sit above DOS core contracts.
4. Modern features are additive and must be disableable.
5. Stage1 is the boot loader; CIUKIDOS is the destination owner for normal DOS runtime services.

## 5. Execution and Memory Model
1. Boot starts in 16-bit real mode.
2. Conventional memory model is required (`0x00000` to `0x9FFFF`).
3. PSP and MCB-compatible process/memory structures are required.
4. UMB/HMA support is optional in early milestones and required later.

## 6. Process and Program Model
1. Support `.COM` loading with `CS=DS=ES=SS` process segment model.
2. Support `.EXE` MZ loading with relocation handling and PSP linkage.
3. Support DOS termination semantics (`INT 20h`, `INT 21h AH=4Ch`).
4. Preserve DOS return-code behavior (`INT 21h AH=4Dh`).

## 7. Interrupt Compatibility Surface
### 7.1 Mandatory Core
1. `INT 21h` (DOS services): file I/O, memory, process control, directory/path, date/time.
2. `INT 10h` (video BIOS compatibility).
3. `INT 13h` (disk BIOS compatibility).
4. `INT 16h` (keyboard BIOS compatibility).
5. `INT 1Ah` (timer/date BIOS compatibility).

### 7.2 Required for GUI and interactive runtime
1. `INT 33h` (mouse driver interface baseline).
2. Stable timer and input behavior for event loops.

## 8. File System Contract
1. `floppy` profile: FAT12 legacy/minimal baseline.
2. `full` and `full-cd` profiles: FAT16 baseline required; FAT32 remains planned future scope.
3. DOS path semantics (`8.3`, canonicalization, root/relative handling) are mandatory.
4. Handle-based file API semantics and error signaling must match DOS expectations.
5. FAT16 sector addressing must preserve the complete 32-bit logical LBA through EDD and legacy-CHS fallback; a low-word-only success path is not conformant.

## 9. Device and Console Contract
1. Standard DOS handle semantics for stdin/stdout/stderr are required.
2. Character device behavior must be deterministic.
3. Console and serial observability are required for debugging and test automation.

## 10. Modern Extension Policy
1. Extensions must not alter default DOS-visible behavior.
2. New features must be opt-in or compatibility-safe by default.
3. Legacy software behavior takes precedence over convenience shortcuts.

## 11. Explicit Non-goals
1. No dependency on UEFI in the new runtime core.
2. No CPU emulation as the final compatibility architecture.
3. No Windows NT+ scope.

## 12. Acceptance Criteria
1. Reproducible full/full-CD boot on BIOS in QEMU and applicable behavior on at least one documented real legacy target.
2. Deterministic DOS interrupt behavior for mandatory core functions.
3. Successful execution of staged DOS compatibility smoke suites.
4. Documented compatibility matrix and regression gates per milestone.
5. A deterministic full-CD fixture above LBA 65,535 must read correctly through the active EDD/CHS32 disk path.
