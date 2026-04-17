# CiukiOS Roadmap - From Current Stage2 to Running DOS DOOM

## Mission
Build a DOS-compatible environment inside CiukiOS capable of running real DOS executables, with the first major game milestone: launching and playing DOOM (DOS).

## North Star (Current Cap)
Run `DOOM.EXE` (or `DOOM2.EXE`) from CiukiOS with keyboard input, VGA graphics, timer/audio interrupts, and stable file I/O.

## Current Snapshot (v0.6.9, updated 2026-04-17)
1. Stage2 shell is active with DOS-like command surface (`dir/type/copy/ren/move/mkdir/rmdir/attrib/del/run`).
2. COM runtime contract is active; EXE MZ path has relocation and edge-case hardening with deterministic host-side regression tests plus real EXE corpus validation gate.
3. INT21 priority-A path includes FAT-backed file handles, file search (`4Eh/4Fh`), rename subset (`56h`), and DOS-like one-shot `AH=4Dh` semantics with matrix gating.
4. Low-level runtime (IDT/PIT/IRQ1 path) now exposes deterministic startup selftests for timer progress and keyboard decode/capture.
5. Video stack includes double-buffering path with dynamic backbuffer budget (up to 1920x1080 Full HD), mode catalog handoff, `vmode` utility, dedicated non-interactive video mode regression tests, backbuffer policy validation, and explicit runtime `1024x768` baseline policy marker.
6. FreeDOS symbiotic pipeline now includes upstream sync orchestration and reproducible runtime-manifest validation for packaging reliability.
7. M6 kickoff artifacts are now active (`docs/m6-dos-extender-requirements.md`, `make test-m6-pmode`, `scripts/test_doom_readiness_m6.sh`).
8. FAT layer advanced toward FAT32-first behavior: mount metadata marker (`type/fsinfo/next_free_hint`), hint-based allocation, and dynamic growth for non-fixed directory chains.
9. SR-VIDEO-001 reached v2 baseline with overlay plane, pacing telemetry, layout metrics and font profiles, covered by `make test-video-ui-v2`, while GUI discoverability/alignment closure is now enforced by stricter `make test-gui-desktop` checks.
10. SR-DOSRUN-001 now has deterministic COM smoke execution (`CIUKSMK.COM`) with explicit run outcome markers and dedicated gate `make test-dosrun-simple`.
11. DOS startup-chain baseline is active: `CONFIG.SYS`, `AUTOEXEC.BAT` and `.BAT` execution (`goto`, `if errorlevel`, env expansion, `set`, `echo`) are wired in stage2 and validated by `make test-startup-chain`.

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
1. Freeze the first milestone target precisely: selected `DOOM` binary, selected WAD set, expected DOS extender (`DOS/4GW` class or variant), and the minimum success checkpoint for the first milestone attempt.
2. Add a first real DOS-extender regression target beyond `CIUK4GW.EXE` and require it to pass farther than pure startup markers.
3. Extend the DPMI host path from `INT 2Fh AX=1687h` into the first usable service slice required by that target extender binary.
4. Validate protected-mode memory handoff above conventional memory (`>1MB` expectations, ownership, overlap safety, return-path integrity).
5. Add dedicated compatibility tests for BIOS interrupt behaviors used by real DOS tools and startup paths (`10h`, `16h`, `1Ah`, `2Fh`) against broader real-binary traces.
6. Close the most likely remaining `INT 21h` runtime gaps surfaced by installer/game startup traces instead of by generic parity guessing.

## Remaining Steps To The Milestone

### A. Freeze the target and its constraints
1. Choose the exact first binary to target (`DOOM.EXE` vs `DOOM2.EXE`, exact package/build, exact DOS extender expectations).
2. Freeze the minimal asset set needed for milestone validation (`DOOM.WAD` or alternative, config defaults, optional launch BAT).
3. Define the first success criterion in order: `binary discovered` -> `extender init passes` -> `video init passes` -> `first frame` -> `main menu` -> `level load`.

### B. Finish the DOS extender path
1. Keep the current `CIUK4GW.EXE` smoke as the shallowest contract test.
2. Add a second-stage DOS extender regression binary that exercises more than host detection.
3. Implement the minimum `DPMI` host behavior required by that binary, not a speculative large surface.
4. Validate real-mode callback and interrupt-reflection behavior against what the chosen extender actually uses.
5. Harden protected-mode memory allocation/ownership rules for the extender load path.
6. Require a regression state stronger than startup markers: the binary must reach an interactive or near-interactive checkpoint.

### C. Close BIOS and DOS runtime gaps used by DOOM startup
1. Capture or infer the startup interrupt/API footprint of the chosen target binary.
2. Expand `INT 10h` compatibility from current baseline to the exact text/video operations used before graphics handoff.
3. Expand `INT 16h` semantics for gameplay-relevant key handling and extended scan-code behavior.
4. Expand `INT 1Ah` timer behavior where startup/runtime depends on BIOS tick semantics.
5. Extend `INT 2Fh` beyond the current minimal smoke path where the target DOS extender/runtime expects it.
6. Fill only the remaining `INT 21h` gaps actually exercised by the target boot/install/runtime path.

### D. Add the graphics path required by DOOM
1. Implement `VGA mode 13h` compatibility as the first graphics milestone.
2. Validate framebuffer layout, palette handling and page semantics required by the target binary.
3. Add deterministic tests that prove mode switch, draw path and return path work without breaking existing GUI/video gates.
4. Add a `VBE` subset only if the selected binary/runtime demonstrably requires it.
5. Define the first graphics success checkpoint as "DOOM draws a real frame/menu" rather than "video API returns success".

### E. Package the game path end-to-end
1. Define deterministic image layout for executable, WAD files, config files and launch scripts.
2. Add launch-path rules so the shell/runtime can discover the game and its assets predictably.
3. Add a boot-to-DOOM harness that checks each stage of startup and classifies the failure point precisely.
4. Document the required files, naming and launch command so the milestone can be reproduced.

### F. Reach playability
1. Tune keyboard path for gameplay latency, repeat behavior and extended keys.
2. Add the first audio-compatible baseline, most likely a Sound Blaster/QEMU-oriented path.
3. Validate that input and audio do not regress the already-stable DOS/video runtime.
4. Require main menu reachability, in-level reachability and a non-trivial gameplay smoke test.

### G. Close the milestone formally
1. Add milestone gates for `main menu reachable`, `level entered`, and `10-minute gameplay smoke`.
2. Publish a playability checklist and a known-gap list.
3. Only then mark the DOOM milestone as complete.

## Critical Path
1. Freeze the target binary/runtime pair.
2. Finish the first usable `DPMI` slice and validate it with a non-trivial extender binary.
3. Add the exact graphics path required by that target (`mode 13h` first).
4. Build the boot-to-DOOM harness.
5. Finish input/audio/playability gates.

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
