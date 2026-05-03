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
8. Donations and support: [DONATIONS.md](DONATIONS.md)

## Current Version
`CiukiOS pre-Alpha v0.5.4`

Versioning policy:
1. Baseline is reset to `pre-Alpha v0.5.0`.
2. Minor updates on this branch keep compatibility with the `v0.5.0` baseline.

## Changelog (Latest 2 Entries)
### Unreleased (2026-05-03)
1. Added a full-profile DOOM taxonomy harness and Makefile target to classify launch progress stages deterministically.
2. Added local-only DOOM payload packaging in the full image build lane and guarded proprietary assets from publication.
3. Fixed INT 21h MZ loading to use header-declared module size, removing the previous 4B:08 launch failure and advancing DOOM to extender startup diagnostics.
4. Closed the Phase 4 installer execution lane with deterministic scenario coverage (success, media swap, timeout, missing media, and insufficient space).
5. Hardened installer manifest-source diagnostics, including explicit `MANIFEST_MEDIA_HEX` reporting for normal and fallback parse paths.
6. Synchronized project documentation to reflect installer-lane closure while keeping the runtime/DOOM lane active.
7. Improved README changelog visibility and updated local agent directives to require a `CHANGELOG.md` update for every completed task.
8. Advanced the DOOM taxonomy harness to boot the full profile interactively, invoke `DRVLOAD.COM`, and launch `DOOM.EXE`, adding a deterministic `doom_exec_attempted` stage before extender/video/menu gates.

### pre-Alpha v0.5.4 (2026-05-01)
1. Improved shell input stability for hold-key repeat, line wrap, and backspace behavior.
2. Stabilized FAT16 shell footer telemetry (`CPU/DSK/RAM`) with corrected non-stuck stat refresh behavior.
3. Revalidated cross-profile build/regression lanes on floppy (FAT12) and full (FAT16) profiles.

Full changelog: [CHANGELOG.md](CHANGELOG.md)

## Current Direction
1. Phase 4 remains active for the runtime/DOOM lane, with shell-first stability as the primary guardrail.
2. The Phase 4 installer execution lane is closed (2026-05-03) with deterministic evidence and scenario coverage.
3. Keep cross-profile runtime stability across floppy/full profiles while advancing DOS compatibility in small, testable steps.
4. Track advanced installer media targets (multi-floppy and extended CD workflow) as post-MVP follow-up scope.

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
5. Migration/archive note: [docs/migration-note-old-archive.md](docs/migration-note-old-archive.md)

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
