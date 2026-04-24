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
8. OpenGEM runtime normalization: [docs/opengem-runtime-normalization.md](docs/opengem-runtime-normalization.md)
9. OpenGEM hardware lane: [docs/opengem-hardware-validation-lane.md](docs/opengem-hardware-validation-lane.md)
10. OpenGEM final closure report: [docs/opengem-final-validation-closure-2026-04-24.md](docs/opengem-final-validation-closure-2026-04-24.md)
11. Hardware evidence: [docs/hardware/opengem-hardware-execution-2026-04-24.md](docs/hardware/opengem-hardware-execution-2026-04-24.md)
10. Full changelog: [CHANGELOG.md](CHANGELOG.md)
11. Donations and support: [DONATIONS.md](DONATIONS.md)

## Current Version
`CiukiOS pre-Alpha v0.5.9-final`

Versioning policy:
1. Baseline is reset to `pre-Alpha v0.5.0`.
2. `-final` suffix marks a closed milestone: all P0/P1/P2 validation gates passed, including real hardware evidence.

## Changelog (Latest)
### pre-Alpha v0.5.9-final (2026-04-24) — OpenGEM milestone closure
1. **OG-P0 gate**: 20 QEMU runs, 100% launch, 100% return-to-shell, 0 hangs → PASS.
2. **OG-P1 soak**: 100 runs × 20 min on QEMU → PASS; hardware evidence on real x86 (HP w19) → PASS.
3. **OG-P1 VDI/AES**: hardened coordinate clipping, stateful INT33h, VDI validation module.
4. **OG-P2 regression lock**: 10 deterministic checks → PASS; perf budget framework → PASS.
5. **Final validation bundle**: gate + acceptance + soak + hardware aggregated → Verdict: PASS.
6. Added CD-ROM profile scaffolding (`build_full_cd.sh`, `full_cd_mbr.asm`).

Full changelog: [CHANGELOG.md](CHANGELOG.md)

## Current Direction
1. OpenGEM milestone **closed** (v0.5.9-final): gate, acceptance, soak, regression lock, perf budget, and hardware evidence all PASS.
2. Next targets: DOOM-on-DOS runtime bring-up, then Windows pre-NT compatibility (up to Windows 98).
3. CD-ROM boot profile in scaffolding — to be completed in the next minor milestone.

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

## OpenGEM P0/P1 Tooling
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
4. `make opengem-soak-full`
	Runs OG-P1-02 long-session soak campaign (default 20 minutes) and writes:
	- `build/full/opengem-soak-full.latest.report.json`
	- `build/full/opengem-soak-full.latest.report.txt`
	- per-run artifacts in `build/full/opengem-soak-latest/`
5. `make opengem-hardware-lane-pack`
	Packages OG-P1-03 hardware lane templates under `build/full/opengem-hardware-lane-latest/`.
6. Runtime/hardware docs:
	- [docs/opengem-runtime-normalization.md](docs/opengem-runtime-normalization.md)
	- [docs/opengem-hardware-validation-lane.md](docs/opengem-hardware-validation-lane.md)
	- [docs/templates/opengem-hardware-execution-template.md](docs/templates/opengem-hardware-execution-template.md)
	- [docs/templates/opengem-hardware-evidence-template.json](docs/templates/opengem-hardware-evidence-template.json)
7. Optional environment overrides:
	- `RUNS=<n>` for acceptance iterations
	- `QEMU_TIMEOUT_SEC=<seconds>` for trace/acceptance timeout
	- `SOAK_DURATION_MIN=<20..30>` for soak duration
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
4. `make opengem-perf-baseline`
	Captures OG-P2-02 baseline metrics in:
	- `build/full/opengem-performance-baseline.latest.json`
5. `make opengem-perf-check`
	Runs periodic budget checks against baseline and writes:
	- `build/full/opengem-performance-budget-check.perfcheck.report.txt`
6. Budget config:
	- [docs/opengem-performance-budget.json](docs/opengem-performance-budget.json)
7. `make opengem-final-bundle`
	Aggregates final milestone evidence from gate, acceptance, soak, and hardware artifacts into:
	- `build/full/opengem-final-validation-bundle.latest.report.txt`
	- `build/full/opengem-final-validation-bundle.latest.report.json`
	Optional labels can be passed by calling the script directly:
	- `bash scripts/opengem_final_validation_bundle.sh --label final --gate-label final-closure --acceptance-label final-closure-acc --soak-label final-closure-soak`

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
