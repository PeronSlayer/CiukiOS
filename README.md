![Splashscreen CiukiOS](misc/CiukiOS_SplashScreen.png)

# CiukiOS

Open Source RetroOS project rebuilt from a clean baseline.
Mission: deliver a native legacy BIOS x86 system able to run DOS and pre-NT software progressively, without CPU emulation in the final runtime path.

## Quick Links
1. Project roadmap: [Roadmap.md](Roadmap.md)
2. Architecture baseline: [docs/architecture-legacy-x86-v1.md](docs/architecture-legacy-x86-v1.md)
3. DOS core spec: [docs/dos-core-spec-v0.1.md](docs/dos-core-spec-v0.1.md)
4. DOS core implementation plan: [docs/dos-core-implementation-plan-v0.1.md](docs/dos-core-implementation-plan-v0.1.md)
5. AI/development directives: [docs/ai-agent-directives.md](docs/ai-agent-directives.md)
6. Engineering logbook: [docs/diario-bordo-v2.md](docs/diario-bordo-v2.md)
7. Shell runtime stability note (2026-04-28): [docs/shell-runtime-stability-2026-04-28.md](docs/shell-runtime-stability-2026-04-28.md)
8. Full changelog: [CHANGELOG.md](CHANGELOG.md)
9. Donations and support: [DONATIONS.md](DONATIONS.md)

## Current Version
`CiukiOS pre-Alpha v0.5.0`

Versioning policy:
1. Baseline is reset to `pre-Alpha v0.5.0`.
2. Minor updates on this branch keep compatibility with the `v0.5.0` baseline.

## Changelog (Latest)
### pre-Alpha v0.5.0 (2026-04-28)
1. Stabilized Stage1 startup and shell-entry runtime behavior for deterministic boot bring-up.
2. Hardened shell prompt/input paths, including drive and current-directory state handling.
3. Improved QEMU stderr observability to speed up runtime diagnostics and regression triage.

Full changelog: [CHANGELOG.md](CHANGELOG.md)

## Current Direction
1. Keep shell-first runtime stable across floppy/full profiles.
2. Prioritize real floppy reliability and deterministic boot behavior.
3. Advance DOS compatibility in small, testable steps.

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
5. AI/development directives: [docs/ai-agent-directives.md](docs/ai-agent-directives.md)
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
