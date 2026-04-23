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
7. OpenGEM completion plan: [docs/opengem-completion-execution-plan-v0.5.9.md](docs/opengem-completion-execution-plan-v0.5.9.md)
8. Full changelog: [CHANGELOG.md](CHANGELOG.md)
9. Donations and support: [DONATIONS.md](DONATIONS.md)

## Current Version
`CiukiOS pre-Alpha v0.5.9`

Versioning policy:
1. Baseline is reset to `pre-Alpha v0.5.0`.

## Changelog (Latest)
### pre-Alpha v0.5.9
1. Fixed DOS I/O carry propagation in `INT 21h AH=3Fh/40h/42h` done paths, removing false read-error reporting during OpenGEM probe flow.
2. Improved OpenGEM GEMVDI probe compatibility by relaxing special `find-next` behavior and reducing premature `0x12` no-more-files returns.
3. Added `VD*` open alias mapping to bundled `SDPSC9.VGA` to keep GEM driver discovery aligned with available runtime payloads.
4. Hardened Stage1 OpenGEM tracing stability while keeping loader payload inside the 29-sector budget.

Full changelog: [CHANGELOG.md](CHANGELOG.md)

## Current Direction
1. Keep deterministic BIOS bring-up and DOS runtime gates green on both `floppy` and `full` profiles.
2. Expand native DOS compatibility incrementally on top of the current Stage1 FAT/file I/O foundation.
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

## OpenGEM P0 Tooling
1. `make opengem-trace-full`
	Generates DOS/OpenGEM tracing artifacts in `build/full/`:
	- `opengem-trace-full.latest.serial.log`
	- `opengem-trace-full.latest.qemu-int.log`
	- `opengem-trace-full.latest.int21-summary.txt`
2. `make opengem-acceptance-full`
	Runs the graphical acceptance loop (default `N=20`) and writes:
	- per-run logs in `build/full/opengem-acceptance-latest/`
	- summary report `build/full/opengem-acceptance-full.latest.report.txt`
3. `make opengem-gate-final`
	Runs the official OG-P0-05 final gate (single verdict PASS/FAIL) and writes:
	- `build/full/opengem-gate-final.latest.report.txt`
	Documentation: [docs/opengem-final-gate-og-p0-05.md](docs/opengem-final-gate-og-p0-05.md)
4. Optional environment overrides:
	- `RUNS=<n>` for acceptance iterations
	- `QEMU_TIMEOUT_SEC=<seconds>` for trace/acceptance timeout
	- `OPENGEM_GATE_LAUNCH_THRESHOLD=<pct>` (default 90)
	- `OPENGEM_GATE_RETURN_THRESHOLD=<pct>` (default 95)
	- `OPENGEM_GATE_MAX_HANGS=<n>` (default derived from return threshold)

## OpenGEM P2 Tooling
1. `make opengem-regression-lock`
	Runs OG-P2-01 historical regression checks (carry, find-next, alias path, memory free/resize signatures)
	and writes: `build/full/opengem-regression-lock.latest.report.txt`
2. Aggregate suite integration:
	`scripts/qemu_test_all.sh` now includes `scripts/qemu_test_opengem_regressions.sh`.
3. Parallel-agent OG-P1 task handoff:
	[docs/opengem-p1-agent-task.md](docs/opengem-p1-agent-task.md)

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
