# Changelog

All notable project-level changes are tracked here.
This changelog is intentionally concise. Every completed task should update `Unreleased` unless the task cuts a release section.

## Unreleased (2026-05-11)

1. Restored the current DOOM/WOLF3D runtime branch around a compact Stage1 MZ/DOS memory layout: primary EXE loading now has a separate MZ load limit, low DOS scratch buffers are isolated from the loader window, MZ tail memory is cleared before handoff, and runtime cache state is reset before external MZ execution.
2. Fixed the DOOM launch regression that stalled at `V_Init: allocate screens`; the dedicated DOOM taxonomy lane now reaches `visual_gameplay=PASS` on the full profile.
3. Fixed WOLF3D black-screen startup in local full-image builds by patching only the injected copy of `WOLF3D.EXE` to bypass the stuck page-flip wait. The source payload under `third_party/WOLF3D` remains untouched, and the final WOLF3D capture is non-black (`720x400`, `colors=6`, `nonblack=24336`).
4. Added SB16 probe/playback evidence through `SB16INIT.COM`, `DRVLOAD.COM /AUDIO`, and QEMU SB16 device wiring. QEMU audio now defaults to on across full, full-CD, taxonomy, and DRVLOAD smoke runners while still allowing `QEMU_AUDIO_MODE=off` for explicit silent runs; the validated path detects the DSP at `0x220`, configures IRQ/DMA mixer state, and completes a real DMA1/IRQ7 SB playback probe. DOOM now launches as `DOOM.EXE`, removing the old `-nosound` and `-nosfx` workarounds while the generated music-only config keeps the unstable SB SFX/DMA path disabled via `snd_channels=0` and `snd_sfxdevice=0`, with the DOOM taxonomy pinned to the ALSA backend to avoid a local PipeWire/OPL QEMU crash, while SB DMA/IRQ sound effects remain follow-up work.
5. Kept the full-CD build inside the Stage1 size budget by making the legacy Stage2 autorun/hardware-validation CD path opt-in instead of forced by default; the active full-CD gate remains the Live/install D: prompt smoke.
6. Restored the README as a complete project entry point, condensed this changelog to release-level facts, and removed temporary local run artifacts while keeping generated build outputs out of the tracked worktree.
7. Improved shell external-command dispatch with GPLv2 FreeCOM-style first-word/rest splitting and `DoExec`-style command-tail packing, so direct `PROGRAM ARG` launches pass the argument tail like the existing `run PROGRAM ARG` path and quoted external command names are parsed without changing prompt text or shell UX; the shared full/full-cd DOS environment now exposes `PATH=C:\APPS;C:\SYSTEM\DRIVERS` for external programs.
8. Relicensed CiukiOS project code from GPLv3 to GPLv2 so GPLv2 FreeDOS/FreeCOM code can be integrated directly, removed the prebuilt FreeCOM binary packaging shortcut, and documented the source-port policy.
9. Added a reversible DOOM audio-profile packaging switch for full/full-cd (`CIUKIOS_DOOM_AUDIO_PROFILE=music-only|sb16`) and a narrow taxonomy pre-launch probe that prints `DEFAULT.CFG` before `run DOOM.EXE`, so runtime investigations can distinguish the intentional music-only bypass from an SB16-enabled launch without changing shell UX or the single-command DOOM run path.
10. Removed the FAT16 shell footer/status strip entirely from Stage1 by turning the footer update/poll path into no-ops and by dropping the remaining decorative horizontal separator lines from the shell chrome, keeping the shell prompt/output UX simpler while recovering Stage1 code budget on both full and full-CD builds.
11. Added `AUDIOTST.COM`, a tiny SB16-focused audio utility that probes the DSP at legacy bases and plays three short DMA-backed test tones with serial/log markers, and packaged it into `\SYSTEM\DRIVERS` for full/full-CD images together with a short `AUDIO.COM` alias for easier invocation from the shell.
12. Fixed bare shell execution of driver helpers from the default `C:\APPS>` prompt by extending Stage1 external-command resolution to probe `\SYSTEM\DRIVERS` in addition to CWD and `\APPS`, so short commands like `AUDIO`, `AUDIOTST`, `SB16INIT`, and `DRVLOAD` no longer require `run ...` or a manual `cd \SYSTEM\DRIVERS`.

Validation evidence:
- `make build-full` PASS.
- `make qemu-test-full` PASS.
- `make verify-full-drivers-payload` PASS with 64 driver files including `SB16INIT.COM`.
- `make qemu-test-full-doom-taxonomy` PASS with `visual_gameplay=PASS`, QEMU audio on, and the DOOM visual gate launched as `run DOOM.EXE` using music-enabled audio config with SFX channels disabled on the ALSA backend.
- WOLF3D full-profile taxonomy PASS at `runtime_stable`, with `build/full/wolf-final-visible.ppm` stats `720x400 colors=6 nonblack=24336`.
- `DRVLOAD_ARGS=/AUDIO QEMU_TIMEOUT_SEC=260 bash scripts/qemu_test_full_drvload_smoke.sh` PASS with QEMU audio on (`backend=pipewire`, SB16 `0x220`, IRQ 7), including `DSP OK at 0x0220`, `DMA DONE`, `TONE DONE`, and `OK AUDIO` markers.
- `make build-full-cd` PASS.
- `CIUKIOS_DOOM_AUDIO_PROFILE=sb16 make build-full` PASS; the injected `build/full/obj/doom-default.cfg` now carries `snd_channels=8`, `snd_sfxdevice=3`, `snd_sbport=544`, `snd_sbirq=7`, `snd_sbdma=1`, and `snd_mport=816`.
- `DO_BUILD=0 CIUKIOS_DOOM_AUDIO_PROFILE=sb16 make qemu-test-full-doom-taxonomy` FAIL at `visual_gameplay` but PASS at `runtime_stable`; serial startup still reaches `I_StartupSound`, `calling DMX_Init`, and `S_Init: Setting up sound`, while also reporting `Dude.. The Adlib isn't responding.` before the screen regresses to a low-diversity frame.
- `CIUKIOS_DOOM_AUDIO_PROFILE=sb16 make build-full`, followed by injecting a no-music DOOM config (`snd_musicdevice=0`, `snd_mport=-1`, `snd_sfxdevice=3`) into `::APPS/DOOM/DEFAULT.CFG` and `::DOOMDATA/DEFAULT.CFG`, then `DO_BUILD=0 make qemu-test-full-doom-taxonomy`: FAIL at `visual_gameplay` but PASS at `runtime_stable`; the serial log still reaches `I_StartupSound`, `calling DMX_Init`, and `S_Init: Setting up sound`, but the AdLib complaint disappears, narrowing the remaining fault away from the music fallback and toward the SB/DMX sound-effects path or its interaction with gameplay rendering.
- `make build-full` PASS after the shell-chrome cleanup and audio-tool integration; `verify-full-drivers-payload` now passes with 66 files including `SB16INIT.COM`, `AUDIOTST.COM`, and the `AUDIO.COM` alias.
- `DO_BUILD=0 DOS_TAXONOMY_USE_CASE=generic DOS_TAXONOMY_PROFILE=dos_generic DOS_TAXONOMY_MIN_STAGE=transfer_marker DOS_TAXONOMY_APP_DIR_IN_IMAGE=::SYSTEM/DRIVERS DOS_TAXONOMY_APP_BINARY_NAME=AUDIOTST.COM DOS_TAXONOMY_CWD='\SYSTEM\DRIVERS' DOS_TAXONOMY_RUN_COMMAND='run AUDIOTST.COM' DOS_TAXONOMY_APP_RUNTIME_MARKERS='\[AUDIOTST\][[:space:]]+DONE' DOS_TAXONOMY_RUN_DRVLOAD=0 QEMU_AUDIO_MODE=on QEMU_AUDIO_BACKEND=alsa QEMU_TIMEOUT_SEC=240 bash scripts/qemu_test_full_dos_taxonomy.sh` PASS at `transfer_marker`; fresh serial output shows `[AUDIOTST] DSP OK at 0x0220`, `TONE 1`, `DMA DONE`, `TONE 2`, `DMA DONE`, `TONE 3`, `DMA DONE`, and `DONE`. The same utility intentionally fails `runtime_stable` because it exits back to the shell after completing.
- `DO_BUILD=0 DOS_TAXONOMY_USE_CASE=generic DOS_TAXONOMY_PROFILE=dos_generic DOS_TAXONOMY_MIN_STAGE=transfer_marker DOS_TAXONOMY_PROMPT_TIMEOUT_SEC=180 DOS_TAXONOMY_APP_DIR_IN_IMAGE=::SYSTEM/DRIVERS DOS_TAXONOMY_APP_BINARY_NAME=AUDIO.COM DOS_TAXONOMY_CWD='\SYSTEM\DRIVERS' DOS_TAXONOMY_RUN_COMMAND='run AUDIO.COM' DOS_TAXONOMY_APP_RUNTIME_MARKERS='\[AUDIOTST\][[:space:]]+DONE' DOS_TAXONOMY_RUN_DRVLOAD=0 QEMU_AUDIO_MODE=on QEMU_AUDIO_BACKEND=alsa QEMU_TIMEOUT_SEC=240 bash scripts/qemu_test_full_dos_taxonomy.sh` PASS at `transfer_marker`; the short `AUDIO.COM` alias reaches the same three-tone `[AUDIOTST]` sequence and then returns to the shell by design.
- `make build-full` PASS after extending Stage1 bare-command resolution to probe `\SYSTEM\DRIVERS`.
- `DO_BUILD=0 IMG=build/full/ciukios-full-audio-bare2.img LOG_FILE=build/full/qemu-audio-bare2.log QEMU_STDERR=build/full/qemu-audio-bare2.stderr QEMU_CMD_LOG=build/full/qemu-audio-bare2.cmd DOS_TAXONOMY_USE_CASE=generic DOS_TAXONOMY_PROFILE=dos_generic DOS_TAXONOMY_MIN_STAGE=transfer_marker DOS_TAXONOMY_PROMPT_TIMEOUT_SEC=180 DOS_TAXONOMY_APP_DIR_IN_IMAGE=::SYSTEM/DRIVERS DOS_TAXONOMY_APP_BINARY_NAME=AUDIO.COM DOS_TAXONOMY_CWD='' DOS_TAXONOMY_RUN_COMMAND='AUDIO' DOS_TAXONOMY_APP_RUNTIME_MARKERS='\[AUDIOTST\][[:space:]]+DONE' DOS_TAXONOMY_RUN_DRVLOAD=0 QEMU_AUDIO_MODE=on QEMU_AUDIO_BACKEND=alsa QEMU_TIMEOUT_SEC=240 bash scripts/qemu_test_full_dos_taxonomy.sh` PASS at `transfer_marker`; the fresh serial log shows bare `AUDIO` typed directly at the default `C:\APPS>` prompt and then emitting the full `[AUDIOTST]` tone sequence through `DONE` before returning to the prompt.
- `make qemu-test-full-dos-compat-smoke` PASS, including DOS21, GFXSTAR, and DOSNavigator startup coverage.
- `make qemu-test-full-cd` PASS with Live CD prompt `D:`.
- `CIUKIOS_DOOM_AUDIO_PROFILE=sb16 make build-full-cd` PASS.
- `make build-full-cd` PASS after the Stage1/footer and audio-tool changes, and `make qemu-test-full-cd` PASS with Live CD prompt `D:`.
- `make qemu-test-full` PASS after removing the shell separator lines from `draw_shell_chrome`.

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
