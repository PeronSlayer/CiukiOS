![Splashscreen CiukiOS](misc/CiukiOS_SplashScreen.png)

# CiukiOS

CiukiOS is a personal open source retro-computing project: a small legacy BIOS x86 operating system rebuilt from a clean baseline.

The long-term goal is to support DOS and pre-NT software progressively, without CPU emulation in the final runtime path. The current system is shell-first, MS-DOS/FreeDOS-inspired, and focused on the FAT16 `full` profile as the main compatibility lane.

CiukiOS is not a finished operating system. It is an active learning and research project, built in spare time with AI-assisted development workflows and a lot of low-level debugging.

## Current Milestone

Current public version: `CiukiOS pre-Alpha v0.6.6`.

The Phase 4 DOOM gameplay milestone is closed. The full FAT16 runtime can launch DOOM through DOS/4GW, load `doom.wad`, initialize the gameplay path, and reach a playable visual runtime.

Current work is concentrated on:

1. continuing the Stage1/runtime split through small runtime-owned service slices
2. improving arbitrary DOS application compatibility from the `full` and `full-cd` profiles
3. hardening DOOM and WOLF3D runtime behavior without regressing the shell or installer lanes
4. bringing up legacy audio in conservative probe stages before claiming playback support
5. keeping validation and release notes focused on reproducible full/full-CD evidence

## Quick Start

Install the usual build and test tools for your platform. On Linux, the active lanes use `nasm`, `mtools`, `xorriso`, `qemu-system-i386` or `qemu-system-x86_64`, Syslinux BIOS files for the full-CD fallback ISO, `python3`, and Python Pillow for splash asset generation.

Build the main FAT16 disk image:

```bash
make build-full
```

Boot-test the main full profile in QEMU:

```bash
make qemu-test-full
```

Build the Live/install CD profile:

```bash
make build-full-cd
```

Smoke-test the Live/install CD profile:

```bash
make qemu-test-full-cd
```

Run the visual Live/install CD profile:

```bash
make qemu-run-full-cd
```

Run the active aggregate validation lane:

```bash
make qemu-test-all
```

Generated images are written under `build/full/`. The main full-profile disk image is `build/full/ciukios-full.img`; the primary Live/install CD image is `build/full/ciukios-full-cd.iso`; the ISOLINUX/memdisk fallback image is `build/full/ciukios-full-cd-isolinux.iso`.

## Active Profiles

| Profile | Status | Purpose | Main commands |
|---|---|---|---|
| `full` | Active default | FAT16 C: disk image, shell-first runtime, DOS compatibility, DOOM/WOLF3D work, driver helper probes | `make build-full`, `make qemu-test-full` |
| `full-cd` | Active install/live media | Bootable Live/install CD, D: shell profile, destructive HDD install flow through `SETUP.COM` | `make build-full-cd`, `make qemu-test-full-cd`, `make qemu-run-full-cd` |
| `floppy` | Legacy/minimal | 1.44MB bring-up and constrained regression work only | `make build-floppy`, `make qemu-test-floppy` |

Default validation should use the `full` lane first. Use `full-cd` when the change touches Live/install media, D: shell behavior, setup, direct ISO boot, or real-hardware install paths. Do not treat the floppy profile as the default release gate unless a task specifically targets it.

## Validation Lanes

Common focused lanes:

```bash
make qemu-test-full
make qemu-test-full-cd
make qemu-test-full-dos-compat-smoke
make qemu-test-full-dos-taxonomy
make qemu-test-full-doom-taxonomy
make qemu-test-full-wolf3d-taxonomy
make qemu-test-full-drvload-smoke
make qemu-test-full-shell-stability
make qemu-test-setup-runtime-hdd-install
```

Use `make qemu-test-all` for the current active aggregate smoke bundle. Focused taxonomy lanes classify runtime stages and should not be upgraded to release claims unless the requested minimum stage and observation window match the claim being made.

## DOS Software And Third-Party Payloads

The repository does not publish commercial DOS game data or proprietary third-party binaries. Local payload directories may be used for private validation only:

1. `third_party/Doom` can be packaged into `C:\APPS\DOOM` when present locally.
2. `third_party/WOLF3D` can be packaged into `C:\APPS\WOLF3D` when present locally.
3. `third_party/DOSNavigator` can be packaged into `C:\APPS\DOSNAV` when present locally.
4. `third_party/drivers` can be packaged into `C:\SYSTEM\DRIVERS` when present locally.

Keep third-party payloads legally supplied, local, and untracked unless a license explicitly permits redistribution. DOSNavigator acknowledgement: "Based on Dos Navigator by RIT Research Labs."

## Audio Status

The full profile now includes a narrow SB16 validation path. `SB16INIT.COM` probes Sound Blaster-compatible DSP bases, verifies the QEMU SB16 DSP at `0x220`, and plays a short direct-DAC tone. `DRVLOAD.COM /AUDIO` runs that helper from `C:\SYSTEM\DRIVERS`.

QEMU full-profile runners support `QEMU_AUDIO_MODE=off|auto|on` and `QEMU_AUDIO_BACKEND=pipewire|pa|pulse|alsa|sdl|none`. The default is `off` so DOS games do not auto-detect an incomplete sound path during compatibility runs; for an audible local probe, set `QEMU_AUDIO_MODE=on` with a host backend such as PipeWire or PulseAudio and then run `\SYSTEM\DRIVERS\DRVLOAD.COM /AUDIO` from the shell.

This is SB16 DSP and direct-DAC tone evidence. Broader DOS game audio, AdLib/OPL behavior, DMA/IRQ playback paths, and DOOM in-game audio remain follow-up compatibility work.

## Project Policy

1. Final runtime compatibility work must not rely on CPU emulation shortcuts.
2. Stage1 size and ownership pressure should be reduced through runtime/module ownership, not endless byte-level feature accretion.
3. Validation claims must name the lane and scope that proved them.
4. Local agent handoffs and transient operational notes belong under `handoff/`, not public docs.
5. Public documentation and changelog entries should stay concise, traceable, and in English.

## Links

1. Full changelog: [CHANGELOG.md](CHANGELOG.md)
2. Project roadmap: [Roadmap.md](Roadmap.md)
3. DOS compatibility matrix: [docs/dos-compatibility-matrix-v0.1.md](docs/dos-compatibility-matrix-v0.1.md)
4. Legacy audio bring-up plan: [docs/legacy-audio-bring-up-plan-v0.1.md](docs/legacy-audio-bring-up-plan-v0.1.md)
5. Setup stream notes: [setup/README.md](setup/README.md)
6. Donations and support: [DONATIONS.md](DONATIONS.md)

## Support

GitHub Sponsors is the primary support channel for CiukiOS: [github.com/sponsors/PeronSlayer](https://github.com/sponsors/PeronSlayer).

Non-monetary help is also useful: reproducible bug reports, focused pull requests, documentation improvements, and compatibility results from real DOS/FreeDOS software are all welcome.
