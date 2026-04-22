# CiukiOS Roadmap - From Current Stage2 to Running DOS DOOM

## Mission
Build a DOS-compatible environment inside CiukiOS capable of running real DOS executables, with the first major game milestone: launching and playing DOOM (DOS).

## North Star (Current Cap)
Run `DOOM.EXE` (or `DOOM2.EXE`) from CiukiOS with keyboard input, VGA graphics, timer/audio interrupts, and stable file I/O.

## Current Snapshot (v0.7.1, updated 2026-04-17)
1. Stage2 shell is active with DOS-like command surface (`dir/type/copy/ren/move/mkdir/rmdir/attrib/del/run`).
2. COM runtime contract is active; EXE MZ path has relocation and edge-case hardening with deterministic host-side regression tests plus real EXE corpus validation gate.
3. INT21 priority-A path includes FAT-backed file handles, file search (`4Eh/4Fh`), rename subset (`56h`), and DOS-like one-shot `AH=4Dh` semantics with matrix gating.
4. Low-level runtime (IDT/PIT/IRQ1 path) now exposes deterministic startup selftests for timer progress and keyboard decode/capture.
5. Video stack includes double-buffering path with dynamic backbuffer budget (up to 1920x1080 Full HD), mode catalog handoff, `vmode` utility, dedicated non-interactive video mode regression tests, backbuffer policy validation, and explicit runtime `1024x768` baseline policy marker.
6. FreeDOS symbiotic pipeline now includes upstream sync orchestration and reproducible runtime-manifest validation for packaging reliability.
7. M6 DPMI smoke chain now includes host-detect + version + raw-mode-bootstrap + allocate-LDT + allocate-memory + free-memory + real-mode interrupt-reflection slices, each covered by a dedicated gate.
8. A VGA mode 13h compatibility scaffold is wired (shell `vga13` + startup marker + `make test-vga13-baseline`); the real draw/render path is pending.
9. BIOS compatibility surface markers for `INT 10h`, `INT 16h`, `INT 1Ah`, and `INT 2Fh` are emitted at boot so DOOM-startup dependencies are greppable.
10. A staged boot-to-DOOM failure-taxonomy harness (`make test-doom-boot-harness`) classifies progress into `binary_found`, `wad_found`, `extender_init`, `video_init`, and `menu_reached` stages; the last stage is deferred until a real DOOM runtime is wired.
11. FAT layer advanced toward FAT32-first behavior: mount metadata marker (`type/fsinfo/next_free_hint`), hint-based allocation, and dynamic growth for non-fixed directory chains.
12. SR-VIDEO-001 reached v2 baseline with overlay plane, pacing telemetry, layout metrics and font profiles, covered by `make test-video-ui-v2`, while GUI discoverability/alignment closure is now enforced by stricter `make test-gui-desktop` checks.
13. SR-DOSRUN-001 now has deterministic COM smoke execution (`CIUKSMK.COM`) with explicit run outcome markers and dedicated gate `make test-dosrun-simple`.
14. DOS startup-chain baseline is active: `CONFIG.SYS`, `AUTOEXEC.BAT` and `.BAT` execution (`goto`, `if errorlevel`, env expansion, `set`, `echo`) are wired in stage2 and validated by `make test-startup-chain`.

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
1. Replace the current bootstrap smoke ceiling (`CIUKLDT.EXE`) with a first real DOS-extender regression target that must progress farther than host queries, raw mode-switch address discovery, and shallow LDT allocate-descriptor slice.
2. Validate protected-mode memory handoff above conventional memory (`>1MB` expectations, ownership, overlap safety, return-path integrity).
3. Add dedicated compatibility tests for BIOS interrupt behaviors used by real DOS tools and startup paths (`10h`, `16h`, `1Ah`, `2Fh`) against broader real-binary traces.
4. Close the most likely remaining `INT 21h` runtime gaps surfaced by installer/game startup traces instead of by generic parity guessing.
5. Start the first `mode 13h` graphics checkpoint for the frozen target so success is measured as a real menu/frame milestone rather than API presence.
6. Extend the new DOOM image-packaging harness into a staged boot-to-game failure taxonomy (`binary found`, `WAD found`, `extender init`, `video init`, `menu reached`).

## Frozen First Target
1. Executable: user-supplied DOS shareware `DOOM.EXE` v1.9.
2. Assets: user-supplied shareware `DOOM1.WAD` as the primary expected IWAD; `DOOM.WAD` may be accepted only as an alias when mapped to the same shareware dataset by the harness/package rules.
3. DOS extender expectation: `DOS/4GW` class runtime behavior.
4. Expected first video path: `VGA mode 13h`.
5. First required milestone checkpoint: main menu reachable.
6. Packaging rule: no public redistribution of `DOOM.EXE`/IWAD assets; the user must supply them locally.

## Remaining Steps To The Milestone

### A. Freeze the target and its constraints
1. First target is fixed to user-supplied shareware `DOOM.EXE` v1.9.
2. Minimal asset set is fixed to user-supplied shareware `DOOM1.WAD` with controlled `DOOM.WAD` alias handling only when the dataset matches.
3. First success criterion is fixed to `main menu reachable`; earlier checkpoints (`binary discovered` -> `extender init passes` -> `video init passes` -> `first frame`) remain failure-taxonomy stages, not the milestone itself.

### B. Finish the DOS extender path
1. Keep the current `CIUK4GW.EXE` smoke as the shallowest contract test.
2. Keep `CIUKDPM.EXE` as the second shallow smoke validating descriptor metadata (`ES:DI`, host-data size) beyond simple presence.
3. Keep `CIUK31.EXE` as the third shallow smoke validating a real callable DPMI host slice (`INT 31h AX=0400h`) after descriptor discovery.
4. Keep `CIUK306.EXE` as the fourth shallow smoke validating the first bootstrap-facing DPMI slice (`INT 31h AX=0306h`) after version discovery.
5. Keep `CIUKLDT.EXE` as the fifth shallow smoke validating the first allocate-LDT-descriptors callable slice (`INT 31h AX=0000h`) after the raw-mode bootstrap slice.
6. Add the next DOS extender regression binary only when it exercises more than host detection, descriptor parsing, version query, raw mode-switch address discovery, and shallow LDT allocate slice.
6. Implement the minimum callable `DPMI` host behavior required by that binary, not a speculative large surface.
7. Validate real-mode callback and interrupt-reflection behavior against what the chosen extender actually uses.
8. Harden protected-mode memory allocation/ownership rules for the extender load path.
9. Require a regression state stronger than startup markers: the binary must reach an interactive or near-interactive checkpoint.

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
1. Deterministic image layout for executable, WAD files, config files and launch scripts is now defined and regression-tested.
2. Launch-path rules now package `DOOM.EXE`, `DOOM1.WAD`, optional `DEFAULT.CFG`, and generated `DOOM.BAT` under `/EFI/CiukiOS` so the shell/runtime can discover the game predictably.
3. Extend the current packaging/discovery harness into a boot-to-DOOM harness that checks each stage of startup and classifies the failure point precisely.
4. Keep the required files, naming and launch command documented so the milestone can be reproduced without redistributing assets.

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
1. Freeze the target binary/runtime pair.+ allocate-LDT 
2. Finish the first usable `DPMI` slice beyond the current version + raw-mode bootstrap callable baseline and validate it with a non-trivial extender binary.
3. Add the exact graphics path required by that target (`mode 13h` first).
4. Build the boot-to-DOOM harness on top of the new deterministic image-packaging baseline.
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
