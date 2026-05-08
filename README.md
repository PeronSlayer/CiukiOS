![Splashscreen CiukiOS](misc/CiukiOS_SplashScreen.png)

# CiukiOS

CiukiOS is a personal open source retro-computing project: a small legacy BIOS x86 operating system rebuilt from a clean baseline.

The long-term goal is to progressively support DOS and pre-NT software without CPU emulation in the final runtime path. The current runtime is command-line first, inspired by MS-DOS/FreeDOS, with a full FAT16 profile used for the main compatibility work.

CiukiOS is not a finished operating system. It is an active learning and research project, built in spare time with AI-assisted development workflows and a lot of low-level debugging.

## Current Milestone
`CiukiOS pre-Alpha v0.6.6`

The Phase 4 DOOM gameplay milestone is closed: the full FAT16 runtime can launch DOOM through DOS/4GW, load `doom.wad`, initialize the gameplay path, and reach a playable visual runtime.

Current work focuses on:
1. advancing the Stage1/runtime split through small runtime-owned service extractions
2. improving DOS compatibility so arbitrary real DOS programs can launch from the full and full-CD profiles
3. bringing DOOM and broader DOS software closer to real legacy audio support
4. hardening shell, CD/HDD install, and validation lanes while keeping runtime stability

## Quick Links
1. Full changelog: [CHANGELOG.md](CHANGELOG.md)
2. Project roadmap: [Roadmap.md](Roadmap.md)
3. Donations and support: [DONATIONS.md](DONATIONS.md)

## Versioning
Versioning policy:
1. Baseline was reset to `pre-Alpha v0.5.0`.
2. The `v0.6.x` line marks the Phase 4 DOOM-playable runtime milestone while keeping the legacy BIOS x86 direction intact.

## Changelog (Latest 2 Entries)
### pre-Alpha v0.6.6 (2026-05-08)
1. Fixed a DOOM runtime regression by restoring the full-profile Stage1 memory map used by the DOS runtime buffers and heap limit.
2. Improved DOS memory manager determinism for AH=48/AH=58 strategy handling while remaining within Stage1 budget constraints.
3. Hardened AH=4Ah resize behavior for fragmented allocations and added a dedicated DOS21 fragmented-resize smoke scenario.
