# CiukiOS Roadmap - From Current Stage2 to Running DOS DOOM

## Mission
Build a DOS-compatible environment inside CiukiOS capable of running real DOS executables, with the first major game milestone: launching and playing DOOM (DOS).

## North Star (Current Cap)
Run `DOOM.EXE` (or `DOOM2.EXE`) from CiukiOS with keyboard input, VGA graphics, timer/audio interrupts, and stable file I/O.

## Current Snapshot (v0.6.1, updated 2026-04-17)
1. Stage2 shell is active with DOS-like command surface (`dir/type/copy/ren/move/mkdir/rmdir/attrib/del/run`).
2. COM runtime contract is active; EXE MZ path has relocation and edge-case hardening with deterministic host-side regression tests plus real EXE corpus validation gate.
3. INT21 priority-A path includes FAT-backed file handles, file search (`4Eh/4Fh`), rename subset (`56h`), and DOS-like one-shot `AH=4Dh` semantics with matrix gating.
4. Low-level runtime (IDT/PIT/IRQ1 path) now exposes deterministic startup selftests for timer progress and keyboard decode/capture.
5. Video stack includes double-buffering path with dynamic backbuffer budget (up to 1920x1080 Full HD), mode catalog handoff, `vmode` utility, dedicated non-interactive video mode regression tests, backbuffer policy validation, and explicit runtime `1024x768` baseline policy marker.
6. FreeDOS symbiotic pipeline now includes upstream sync orchestration and reproducible runtime-manifest validation for packaging reliability.
7. M6 kickoff artifacts are now active (`docs/m6-dos-extender-requirements.md`, `make test-m6-pmode`, `scripts/test_doom_readiness_m6.sh`).

## Compatibility Definition for This Goal
1. Execute real `.COM` and `.EXE MZ` binaries.
2. Support enough DOS API (`INT 21h`) for DOOM installer/runtime path.
3. Provide BIOS compatibility layer required by DOS extender and game runtime.
4. Provide protected-mode transition path used by DOS extenders (DOS/4GW class).
5. Keep reproducible automated boot + compatibility tests.

## Architectural Strategy
1. Keep UEFI loader and current Stage2 as modern bootstrap/debug shell.
2. Add a dedicated DOS runtime core (real-mode compatible semantics).
3. Integrate FreeDOS components symbiotically as compatibility userland (where legally and technically suitable).
4. Add a protected-mode game path for DOS extenders and 32-bit code.
5. Build compatibility first, then optimize performance.

## Milestones

### M0 - Baseline Freeze and Test Discipline
Deliverables:
1. Stable docs, deterministic build, CI smoke tests.
2. Golden boot logs for regression checks.
3. Compatibility matrix scaffold for DOS APIs.
Exit criteria:
1. Every merge preserves `test-stage2` and `test-fallback` pass.
2. All new subsystems include minimal automated tests.

### M1 - True DOS Program Execution Core
Deliverables:
1. Real `.COM` loader with PSP semantics.
2. Real `.EXE MZ` loader with relocation handling.
3. Process termination and return code handling (`INT 20h`, `AH=4Ch`).
Exit criteria:
1. 10+ tiny DOS test binaries run and return correctly.

### M2 - Memory Model Required by Real DOS Apps
Deliverables:
1. Conventional memory manager with MCB-compatible behavior.
2. `INT 21h` memory APIs (`48h`, `49h`, `4Ah`) complete and tested.
3. Initial XMS provider strategy (own implementation or FreeDOS component integration).
Exit criteria:
1. Memory stress suite passes with fragmentation scenarios.

### M3 - Filesystem and Handle/FCB Compatibility
Deliverables:
1. FAT12/16 read/write complete with DOS path semantics.
2. Core handle APIs (`3Ch-42h`) and error code compatibility.
3. Basic FCB compatibility layer for legacy programs.
Exit criteria:
1. DOS file utility subset passes (`DIR`, `TYPE`, `COPY`, `DEL`).
2. Regression tests validate timestamps, attributes, seek semantics.

### M4 - Interrupt and Device Compatibility Layer
Deliverables:
1. `INT 10h` text-mode baseline compatibility.
2. `INT 16h` keyboard behavior compatibility.
3. `INT 1Ah` timer/clock compatibility.
4. `INT 2Fh` multiplex baseline for TSR/driver ecosystem.
Exit criteria:
1. Diagnostic DOS tools relying on BIOS calls execute correctly.

### M5 - Command Interpreter Compatibility
Deliverables:
1. `COMMAND.COM`-compatible shell flow.
2. `AUTOEXEC.BAT` and `CONFIG.SYS` startup chain.
3. Environment block and `%VAR%` expansion behavior.
Exit criteria:
1. Interactive DOS workflow works without custom shell fallback.

### M6 - Protected Mode and DOS Extender Path
Deliverables:
1. Protected-mode entry/return path compatible with DOS extenders.
2. Required interfaces for DOS/4GW-style runtime expectations.
3. 32-bit executable validation harness.
Exit criteria:
1. At least one non-trivial DOS extender app runs to interactive mode.

### M7 - Graphics for DOOM Path
Deliverables:
1. VGA mode `13h` compatibility and framebuffer semantics.
2. VBE subset if required by selected DOOM binary variant.
3. Deterministic timing for frame pacing.
Exit criteria:
1. DOOM can initialize video and draw first frame.

### M8 - Audio and Input for Playability
Deliverables:
1. Keyboard input latency and key repeat behavior tuned for games.
2. Sound Blaster-compatible baseline path (or compatible emulation target for QEMU setup).
3. IRQ/DMA behavior sufficient for DOOM sound path.
Exit criteria:
1. DOOM runs with stable input and at least digital SFX.

### M9 - DOOM Execution Milestone
Deliverables:
1. Boot-to-game scripted path from CiukiOS image.
2. Documentation for required files and launch commands.
3. Playability checklist and known compatibility gaps.
Exit criteria:
1. DOOM main menu reachable.
2. Entering a level works without fatal crash.
3. 10-minute gameplay smoke test passes.

### M10 - Hardening and Compatibility Expansion
Deliverables:
1. Broader DOS app/game compatibility suite.
2. Performance profiling and optimization.
3. Optional advanced memory features (HMA/UMB/EMS) and TSR ecosystem improvements.
Exit criteria:
1. Repeatable release profile with compatibility report.

## Immediate Execution Queue (Next 6 Technical Steps)
1. Close remaining `INT 21h` parity gaps: strict CF/AX edge behavior and memory ownership metadata hardening.
2. Add dedicated compatibility tests for BIOS interrupt behaviors used by real DOS tools (`10h`, `16h`, `1Ah`).
3. Start `COMMAND.COM`-compatible startup chain baseline (`CONFIG.SYS` + `AUTOEXEC.BAT`).
4. Implement `.BAT` parser MVP with core control flow and environment expansion subset.
5. Define and implement protected-mode transition contract for DOS extender support (DOS/4GW path).
6. Build first DOOM-focused compatibility harness (boot-to-launch checks before full playability).

## Test Strategy
1. Unit-style runtime tests for APIs and flags.
2. Integration tests booting sample DOS binaries.
3. Golden-log checks for key boot/runtime markers.
4. Game-focused smoke tests for DOOM startup path.

## Risks
1. DOS extenders are the hardest compatibility point.
2. Timing-sensitive behavior can differ between emulators and real hardware.
3. Audio stack and DMA/IRQ correctness may become a long-tail effort.

## Done Definition for the Current North Star
"Done" for this phase means: a user can boot CiukiOS, run a real DOS DOOM executable from the image, and play a level with stable controls and no immediate crash.
