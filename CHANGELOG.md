# Changelog

All notable project-level changes are tracked here.
This changelog is intentionally concise. Every completed task should update `Unreleased` unless the task cuts a release section.

## Unreleased (2026-05-11)

1. Restored the current DOOM/WOLF3D runtime branch around a compact Stage1 MZ/DOS memory layout: primary EXE loading now has a separate MZ load limit, low DOS scratch buffers are isolated from the loader window, MZ tail memory is cleared before handoff, and runtime cache state is reset before external MZ execution.
2. Fixed the DOOM launch regression that stalled at `V_Init: allocate screens`; the dedicated DOOM taxonomy lane now reaches `visual_gameplay=PASS` on the full profile.
3. Fixed WOLF3D black-screen startup in local full-image builds by patching only the injected copy of `WOLF3D.EXE` to bypass the stuck page-flip wait. The source payload under `third_party/WOLF3D` remains untouched, and the final WOLF3D capture is non-black (`720x400`, `colors=6`, `nonblack=24336`).
4. Added SB16 probe/playback evidence through `SB16INIT.COM`, `DRVLOAD.COM /AUDIO`, QEMU SB16 device wiring for the full-profile runner and DOS taxonomy harness, and an audio-aware DRVLOAD smoke path. The validated path detects the DSP at `0x220` and completes a direct-DAC tone.
5. Kept the full-CD build inside the Stage1 size budget by making the legacy Stage2 autorun/hardware-validation CD path opt-in instead of forced by default; the active full-CD gate remains the Live/install D: prompt smoke.
6. Restored the README as a complete project entry point, condensed this changelog to release-level facts, and removed temporary local run artifacts while keeping generated build outputs out of the tracked worktree.

Validation evidence:
- `make build-full` PASS.
- `make qemu-test-full` PASS.
- `make verify-full-drivers-payload` PASS with 64 driver files including `SB16INIT.COM`.
- `DO_BUILD=0 make qemu-test-full-doom-taxonomy` PASS with `visual_gameplay=PASS`.
- WOLF3D full-profile taxonomy PASS at `runtime_stable`, with `build/full/wolf-final-visible.ppm` stats `720x400 colors=6 nonblack=24336`.
- `DO_BUILD=0 DRVLOAD_ARGS=/AUDIO QEMU_TIMEOUT_SEC=260 bash scripts/qemu_test_full_drvload_smoke.sh` PASS, including `DSP OK at 0x0220`, `TONE DONE`, and `OK AUDIO` markers.
- `make build-full-cd` PASS.
- `make qemu-test-full-dos-compat-smoke` PASS, including DOS21, GFXSTAR, and DOSNavigator startup coverage.
- `make qemu-test-full-cd` PASS with Live CD prompt `D:`.

## pre-Alpha v0.6.6 (2026-05-08)

1. Rebased the roadmap after Phase 4: DOOM visual gameplay is closed, while Stage1/runtime split work, broader DOS app compatibility, legacy audio, and full/full-CD hardening are the next priorities.
2. Advanced the Stage1/runtime split foundation with `\SYSTEM\RUNTIME.BIN`, runtime service-table probing, callable service ids 1-5, corrupt-runtime fallback checks, default-drive state bridging, and Stage1 size recovery.
3. Hardened full-profile DOS compatibility across C:/D: drive state, per-drive CWD, FAT16 path/create/open/delete/rename behavior, INT 21h country/IOCTL/switch/PSP/handle/memory services, and external app return-state handling.
4. Expanded external DOS application evidence with CIUKEDIT/GFXSTAR smoke coverage, optional DOSNavigator packaging/startup validation, shell chrome isolation, temporary INT 10h/INT 33h external-app handling, and DOSNavigator-focused stability fixes.
5. Restored and revalidated DOOM startup/gameplay after memory-map and allocator regressions; improved DOOM/DOS taxonomy lanes with honest `runtime_stable` and `visual_gameplay` classification.
6. Hardened the full-CD Live/install path with the direct El Torito ISO as primary output, visual SETUP UI, destructive HDD install flow, `FORMAT.COM`, topology guards, eject-before-reboot prompt, batched install I/O, and ThinkPad T23 real-hardware follow-ups.
7. Added public docs for DOS compatibility and legacy audio planning, updated support links, cleaned obsolete project artifacts, and aligned release-facing metadata for `CiukiOS pre-Alpha v0.6.6`.

## pre-Alpha v0.6.5 (2026-05-05)

1. Established the Stage1/runtime split as the structural direction: Stage1 remains loader-first while runtime, shell, driver/CD policy, diagnostics, and module responsibilities migrate toward loaded components under `\SYSTEM`.
2. Added the inert `src/runtime/runtime.asm` artifact, packaged it as `\SYSTEM\RUNTIME.BIN`, documented the split plan, and kept default full/full-CD boot behavior stable.
3. Validated the slice across active full/full-CD build, QEMU smoke, shell, driver, setup, Stage1 selftest, runtime-probe, and DOOM taxonomy lanes.
4. Updated version, banner, ISO label, README, roadmap, and release metadata for `CiukiOS pre-Alpha v0.6.5`.

## pre-Alpha v0.6.3 (2026-05-05)

1. Promoted the full-CD profile into the main Live/install media path with direct El Torito hard-disk boot, D: live shell behavior, SETUP destructive install support, and disposable HDD install validation.
2. Added CHS/EDD boot and setup hardening, CD/HDD probe lanes, installed-HDD boot checks, and user-facing QEMU run/test targets for the full-CD profile.
3. Improved shell and FAT16 DOS behavior around drive semantics, current-directory preservation, prompt recovery, case handling, footer telemetry, and repeated COM/EXE execution stability.
4. Added DOOM `runtime_stable` taxonomy classification so post-video observation failures are reported honestly instead of being hidden behind earlier startup stages.

## pre-Alpha v0.6.1 (2026-05-04)

1. Closed Phase 4 as DOOM gameplay playable on the full FAT16 profile: DOOM launches through DOS/4GW, loads WAD data, reaches video/gameplay runtime, and was manually confirmed playable by the project owner.
2. Added the full-profile DOOM taxonomy harness, local-only DOOM payload packaging, and staged runtime fixes across MZ loading, FAT16 read/seek behavior, PSP/MCB setup, DOS memory strategy, XMS move support, and DOS extender startup classification.
3. Closed the Phase 4 installer execution lane with deterministic setup scenario coverage and release-facing documentation updates.

## pre-Alpha v0.5.4 (2026-05-01)

1. Improved shell input stability for hold-key repeat, line wrap, and backspace behavior.
2. Stabilized FAT16 shell footer telemetry and revalidated cross-profile build/regression lanes.

## pre-Alpha v0.5.3 (2026-04-30)

1. Stabilized shell `move`/`mv` behavior across floppy and full runtime profiles.
2. Fixed `INT 21h AH=56h` rename/move behavior and revalidated shell command regression lanes.

## pre-Alpha v0.5.2 (2026-04-29)

1. Closed Stage1 DOS command regressions across floppy and full profiles.
2. Stabilized critical INT 21h read/write/seek return paths and floppy image write behavior.

## pre-Alpha v0.5.0 (2026-04-28)

1. Stabilized Stage1 startup, shell entry, prompt/input paths, drive/CWD state, and QEMU stderr observability for more deterministic bring-up.

## pre-Alpha v0.5.0 (2026-04-22)

1. Restarted the project from a clean legacy BIOS x86 architecture baseline.
2. Introduced the initial `floppy` and `full` build profiles with build/QEMU smoke scripts and baseline branch/documentation discipline.
