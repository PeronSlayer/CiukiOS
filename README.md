![Splashscreen CiukiOS](misc/CiukiOS_SplashScreen.png)

# CiukiOS

CiukiOS is a personal open source retro-computing project: a small legacy BIOS x86 operating system rebuilt from a clean baseline.

The long-term goal is to progressively support DOS and pre-NT software without CPU emulation in the final runtime path. The current runtime is command-line first, inspired by MS-DOS/FreeDOS, with a full FAT16 profile used for the main compatibility work.

CiukiOS is not a finished operating system. It is an active learning and research project, built in spare time with AI-assisted development workflows and a lot of low-level debugging.

## Current Milestone
`CiukiOS pre-Alpha v0.6.5`

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
### pre-Alpha v0.6.5 (2026-05-05)
1. Established the Stage1/runtime split path by documenting Stage1 responsibilities, target loader/runtime boundaries, ABI assumptions, migration order, and acceptance gates.
2. Added the first external runtime landing artifact, `\SYSTEM\RUNTIME.BIN`, built from `src/runtime/runtime.asm` and packaged by the full/full-CD build path without changing boot behavior.
3. Bumped the project version to `CiukiOS pre-Alpha v0.6.5` to mark the structural architecture milestone while preserving current full/full-CD runtime behavior.

### pre-Alpha v0.6.3 (2026-05-05)
1. Promoted the full-CD Live/install path with D: prompt validation, full-CD QEMU runner support, and direct ISO smoke coverage.
2. Stabilized the full-profile shell return path after DOS program exit, added the `woof` cd alias, corrected FREE/CPU footer telemetry, and expanded shell stability QEMU coverage.
3. Hardened FAT16 INT 21h path case handling and C:/D: drive/free-space semantics while preserving DOOM runtime stability through taxonomy validation.

Full changelog: [CHANGELOG.md](CHANGELOG.md)

## Current Direction
1. Phase 4 is closed as of 2026-05-04: installer execution is complete and DOOM is playable on the full FAT16 profile.
2. The immediate architecture priority after v0.6.5 is the Stage1/runtime split: keep Stage1 loader-first and move low-risk runtime ownership into `\SYSTEM\RUNTIME.BIN` one slice at a time.
3. The next product milestone after the split foundation is broad DOS application compatibility: CiukiOS should launch a wider set of real DOS programs directly from the full and full-CD images, not only curated demos.
4. Legacy audio support for DOOM and other DOS software is the next major compatibility milestone after broader program bring-up.
5. Networking and Windows pre-NT remain later milestones; the Windows 3.11-style GUI demo is exploratory only and not on the critical path.

## Project Scope
CiukiOS is currently useful as:
1. a retro-computing experiment
2. a learning project around BIOS boot, FAT filesystems, DOS APIs, MZ loading, and protected-mode compatibility
3. a testbed for AI-assisted low-level development workflows

It is not yet intended as a polished daily-use operating system.

## Open Source Collaboration
CiukiOS welcomes collaboration through issues and pull requests.
When proposing work, include:
1. clear problem statement
2. expected behavior
3. reproducible technical context

## Development Pace
This is a spare-time project. Progress is continuous, but not tied to a fixed release calendar.

## Pre-Alpha Policy
1. `main` is protected by branch-based workflow and explicit merge approval.
2. The project is in active architecture and runtime bring-up.
3. Build artifacts are currently engineering scaffolds unless stated otherwise.

## Key Docs
1. Roadmap: [Roadmap.md](Roadmap.md)
2. Architecture baseline: [docs/architecture-legacy-x86-v1.md](docs/architecture-legacy-x86-v1.md)
3. DOS core spec: [docs/dos-core-spec-v0.1.md](docs/dos-core-spec-v0.1.md)
4. DOS core implementation plan: [docs/dos-core-implementation-plan-v0.1.md](docs/dos-core-implementation-plan-v0.1.md)
5. Post-runtime-split roadmap: [docs/post-runtime-compatibility-roadmap-v0.1.md](docs/post-runtime-compatibility-roadmap-v0.1.md)
6. Phase 4 DOOM playable milestone: [docs/phase4-doom-gameplay-playable-2026-05-04.md](docs/phase4-doom-gameplay-playable-2026-05-04.md)

## Donations and Support
CiukiOS is a personal open source project. If you want to support development costs such as AI tooling, test workflows, and maintenance time, you can use [GitHub Sponsors](https://github.com/sponsors/PeronSlayer) or see [DONATIONS.md](DONATIONS.md).

Non-monetary help is also welcome: bug reports, focused pull requests, documentation improvements, and compatibility test results are all useful.

## Credits
Developed collaboratively with AI-assisted workflows and human-driven architecture decisions.

The name **CiukiOS** comes from a private joke between me and my girlfriend about our dog Jack (Jacky), who is no longer with us.
His nickname was **Ciuk/Ciuki**, and we used to joke that if we ever built an operating system, we would call it **CiukiOS**.

That is why this project is dedicated to one of the best dogs I ever met: Jack.
