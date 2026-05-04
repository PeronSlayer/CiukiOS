![Splashscreen CiukiOS](misc/CiukiOS_SplashScreen.png)

# CiukiOS

Open Source RetroOS project rebuilt from a clean baseline.
Mission: deliver a native legacy BIOS x86 system able to run DOS and pre-NT software progressively, without CPU emulation in the final runtime path.

## Quick Links
1. Project roadmap: [Roadmap.md](Roadmap.md)
2. Architecture baseline: [docs/architecture-legacy-x86-v1.md](docs/architecture-legacy-x86-v1.md)
3. DOS core spec: [docs/dos-core-spec-v0.1.md](docs/dos-core-spec-v0.1.md)
4. DOS core implementation plan: [docs/dos-core-implementation-plan-v0.1.md](docs/dos-core-implementation-plan-v0.1.md)
5. Engineering logbook: [docs/diario-bordo-v2.md](docs/diario-bordo-v2.md)
6. Shell runtime stability note (2026-04-28): [docs/shell-runtime-stability-2026-04-28.md](docs/shell-runtime-stability-2026-04-28.md)
7. Full changelog: [CHANGELOG.md](CHANGELOG.md)
8. Phase 4 DOOM playable milestone: [docs/phase4-doom-gameplay-playable-2026-05-04.md](docs/phase4-doom-gameplay-playable-2026-05-04.md)
9. Donations and support: [DONATIONS.md](DONATIONS.md)

## Current Version
`CiukiOS pre-Alpha v0.6.3`

Versioning policy:
1. Baseline was reset to `pre-Alpha v0.5.0`.
2. The `v0.6.x` line marks the Phase 4 DOOM-playable runtime milestone while keeping the legacy BIOS x86 direction intact.

## Changelog (Latest 2 Entries)
### pre-Alpha v0.6.3 (2026-05-05)
1. Promoted the full-CD Live/install path with D: prompt validation, full-CD QEMU runner support, and direct ISO smoke coverage.
2. Stabilized the full-profile shell return path after DOS program exit, added the `woof` cd alias, corrected FREE/CPU footer telemetry, and expanded shell stability QEMU coverage.
3. Hardened FAT16 INT 21h path case handling and C:/D: drive/free-space semantics while preserving DOOM runtime stability through taxonomy validation.

### pre-Alpha v0.6.1 (2026-05-04)
1. Closed the Phase 4 DOOM gameplay milestone: the full FAT16 runtime reaches DOS/4GW, loads `doom.wad`, initializes the gameplay path, renders the viewport/HUD, and has been manually confirmed playable.
2. Reworked the full-profile INT 21h memory arena around an ordered MCB table and fixed DOS extender compatibility issues in MZ sizing, PSP/MCB visibility, AH=33h, FAT16 read/seek returns, and WAD discovery.
3. Added a visual DOOM taxonomy lane with QEMU `-display none` plus optional monitor `screendump` capture, while keeping serial `menu_reached` classification conservative.

Full changelog: [CHANGELOG.md](CHANGELOG.md)

## Current Direction
1. Phase 4 is closed as of 2026-05-04: installer execution is complete and DOOM is playable on the full FAT16 profile.
2. Keep the full-profile runtime stable while preserving the conservative taxonomy split between serial `menu_reached` and visual gameplay evidence.
3. Phase 5 remains the next major compatibility direction after v0.6.3: Windows pre-NT bootstrap/runtime work, starting from the DOS extender and protected-mode compatibility gains proven by DOOM.
4. Track audio, driver activation, and richer gameplay taxonomy as follow-up hardening rather than Phase 4 blockers.

## Open Source Collaboration
CiukiOS welcomes collaboration through issues and pull requests.
When proposing work, include:
1. clear problem statement
2. expected behavior
3. reproducible technical context

## Development Pace
This is a spare-time project.
Progress is continuous but not tied to a fixed release calendar.

## Pre-Alpha Policy
1. `main` is protected by branch-based workflow and explicit merge approval.
2. The project is in active architecture and runtime bring-up.
3. Build artifacts are currently engineering scaffolds unless stated otherwise.

## Key Docs
1. Roadmap: [Roadmap.md](Roadmap.md)
2. Architecture baseline: [docs/architecture-legacy-x86-v1.md](docs/architecture-legacy-x86-v1.md)
3. DOS core spec: [docs/dos-core-spec-v0.1.md](docs/dos-core-spec-v0.1.md)
4. DOS core implementation plan: [docs/dos-core-implementation-plan-v0.1.md](docs/dos-core-implementation-plan-v0.1.md)
5. Phase 4 DOOM playable milestone: [docs/phase4-doom-gameplay-playable-2026-05-04.md](docs/phase4-doom-gameplay-playable-2026-05-04.md)
6. Migration/archive note: [docs/migration-note-old-archive.md](docs/migration-note-old-archive.md)

## Legacy Archive
Historical project content is preserved under:
`OLD/archive-2026-04-22/`

This archive includes prior implementation, docs, and build history for reference.

## Donations and Support
If you want to support CiukiOS development, see:
- [DONATIONS.md](DONATIONS.md)

## Credits
Developed collaboratively with AI-assisted workflows and human-driven architecture decisions.

The name **CiukiOS** comes from a private joke between me and my girlfriend about our dog Jack (Jacky), who is no longer with us.
His nickname was **Ciuk/Ciuki**, and we used to joke that if we ever built an operating system, we would call it **CiukiOS**.

So this is why is dedicated to one of the best dogs i ever met, Jack.
