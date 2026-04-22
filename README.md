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
7. Full changelog: [CHANGELOG.md](CHANGELOG.md)
8. Donations and support: [DONATIONS.md](DONATIONS.md)

## Current Version
`CiukiOS pre-Alpha v0.5.0`

Versioning policy:
1. Baseline is reset to `pre-Alpha v0.5.0`.
2. Version bumps are patch-only (`x.x.1`) and happen only on explicit user instruction.

## Changelog (Latest)
### pre-Alpha v0.5.0
1. Restarted the project on a legacy BIOS x86 architecture baseline.
2. Archived previous project state in `OLD/archive-2026-04-22/`.
3. Added dual build profiles (`floppy`, `full`) and QEMU smoke-test entry scripts.
4. Added operating directives for branch workflow, approval-gated merges, and concise English documentation.

Full changelog: [CHANGELOG.md](CHANGELOG.md)

## Current Direction
1. Build a real BIOS boot path for the `floppy` profile.
2. Implement native DOS runtime compatibility incrementally.
3. Reach OpenGEM and DOOM milestones, then progress toward Windows pre-NT compatibility (up to Windows 98).

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
