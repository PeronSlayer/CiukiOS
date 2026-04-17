# CLAUDE.md - CiukiOS Collaboration Readme

## Purpose
Shared operating context for Codex and Claude Code.
Use this file to stay aligned between sessions, beyond detailed handoff files.

## Project North Star
Current top objective: run real DOS DOOM binaries from CiukiOS.

Primary roadmap:
1. `docs/roadmap-ciukios-doom.md`
2. `docs/roadmap-dos62-compat.md`

## Current Runtime Snapshot
1. UEFI loader boots Stage2 reliably.
2. Stage2 has shell, FAT read/write-on-cache primitives, COM catalog support.
3. M1 DOS-like COM runtime contract is active: PSP-style layout at `+0x100`, command tail propagation, and explicit terminate semantics (`INT 20h` / `INT 21h AH=4Ch` via services ABI).
4. Splashscreen is integrated and rendered at boot.
5. Symbiotic FreeDOS pipeline exists (`scripts/sync_freecom_repo.sh`, `scripts/build_freecom.sh`, `scripts/import_freedos.sh`, `third_party/freedos/runtime/`).
6. Fallback path to `kernel.elf` remains tested.

## Source of Truth Documents
1. High-level roadmap: `docs/roadmap-ciukios-doom.md`
2. Compatibility baseline: `docs/int21-priority-a.md`
3. Phase history/kickoff: `docs/phase-0-kickoff.md`, `docs/phase-1.md`
4. FreeDOS licensing/integration policy: `docs/freedos-integration-policy.md`
5. FreeDOS symbiotic architecture: `docs/freedos-symbiotic-architecture.md`
6. Central durable documentation: `documentation.md`
7. Handoff index: `docs/handoffs/README.md`

## Session Workflow (Required)
1. Read this file first.
2. Read the latest relevant handoff(s) in `docs/handoffs/`.
3. Execute scoped change.
4. Run relevant tests.
5. Write a new handoff for major multi-file or architectural changes.
6. Update `documentation.md` whenever the completed task changes stable project state, architecture, validation flow, or milestone status.
7. Update this file only if global direction/state changed.

## Versioning Cadence Rule (Required)
1. Current baseline version: `CiukiOS Alpha v0.8.0`.
2. Every 3-4 completed roadmap tasks, bump patch version by `+0.0.1`.
3. Example progression: `v0.6.0 -> v0.6.1 -> v0.6.2`.
4. On each version bump, update all of:
	- `README.md` (Current Version + Changelog)
	- `stage2/include/version.h`
	- relevant roadmap/status docs if user-visible scope changed
5. Keep bump cadence deterministic: do not skip changelog updates after a bump.

## Handoff Rule
For major changes, always add one file:
`docs/handoffs/YYYY-MM-DD-<topic>.md`

Minimum handoff content:
1. Context and goal.
2. Files touched.
3. Decisions made.
4. Validation performed.
5. Risks and next step.

## Compatibility Execution Priorities
1. Real DOS binary loading (`.COM`, `.EXE MZ`).
2. Accurate `INT 21h` behavior and error flags.
3. BIOS interrupt compatibility used by real software.
4. Protected-mode path for DOS extender workflows.
5. DOOM graphics/input/audio milestone.

## Build/Test Guardrails
1. Keep `make test-stage2` green.
2. Keep `make test-fallback` green.
3. Do not silently break loader/stage2 ABI.
4. Preserve reproducibility of generated artifacts.

## Asset and Licensing Rules
1. Prefer FreeDOS components with verified licenses for distributable images.
2. Keep Microsoft DOS files user-supplied and out of public redistribution by default.
3. Maintain provenance and licenses for all imported third-party DOS files.
4. Keep `third_party/freedos/manifest.csv` updated via `scripts/import_freedos.sh`.

## Quick Resume Checklist
1. `git status --short`
2. Read newest handoff in `docs/handoffs/`
3. Open `docs/roadmap-ciukios-doom.md`
4. Pick next milestone item and implement with tests

## Last Updated
2026-04-15
