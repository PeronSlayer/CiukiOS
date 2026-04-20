# CLAUDE.md - CiukiOS Collaboration Readme

## Purpose
Shared operating context for Codex and Claude Code.
Use this file to stay aligned between sessions, beyond detailed handoff files.

Collaboration directives that override generic agent defaults live in `docs/agent-directives.md`.
The shared local coordination diary is `docs/collab/diario-di-bordo.md` and must remain untracked.

## Project North Star
Current top objective: run real DOS DOOM binaries from CiukiOS.

Long-term objective (formalized 2026-04-19): support the Windows DOS-based family
(Windows 1.x / 2.x / 3.x / 9x / ME) on top of the shared 16-bit execution
layer designed in `docs/opengem-016-design.md`.

Permanent non-goal: Windows NT / 2000 / XP / Vista / 7 / 8 / 10 / 11 and
ReactOS. These are independent OS kernels and will never be supported by
this project. Any request in that direction is out of scope.

Permanent target architecture (formalized 2026-04-20): CiukiOS is a
**32-bit (IA-32 / i686) operating system**. 64-bit / long-mode / x86_64
is NOT a project goal. The current long-mode stage2 is a historical
artefact of early bring-up and will be phased out in favour of a pure
32-bit protected-mode kernel. New architectural work must target IA-32
and must not introduce new long-mode-only dependencies.

Permanent firmware target (formalized 2026-04-20): the long-term boot
path is **legacy BIOS + MBR**, not UEFI. UEFI-only assumptions (e.g.
Boot Services, GOP-only framebuffer acquisition, long-mode handoff ABI)
must not be baked into new code. A legacy BIOS/MBR bootloader is on the
roadmap; until it lands, the current UEFI loader is kept as a transitional
bring-up vehicle only.

Primary roadmap:
1. `docs/roadmap-ciukios-doom.md`
2. `docs/roadmap-windows-dosbased.md`
3. `docs/roadmap-dos62-compat.md`

Architectural design (prerequisite for both roadmap 1 and roadmap 2 beyond
DOS-only software): `docs/opengem-016-design.md` — v8086 monitor hosted
via 32-bit protected-mode compatibility task from long mode. Implementation
is gated on explicit user approval of that design document.

## Current Runtime Snapshot
1. UEFI loader boots Stage2 reliably.
2. Stage2 has shell, FAT read/write-on-cache primitives, COM catalog support.
3. M1 DOS-like COM runtime contract is active: PSP-style layout at `+0x100`, command tail propagation, and explicit terminate semantics (`INT 20h` / `INT 21h AH=4Ch` via services ABI).
4. Splashscreen is integrated and rendered at boot.
5. Symbiotic FreeDOS pipeline exists (`scripts/sync_freecom_repo.sh`, `scripts/build_freecom.sh`, `scripts/import_freedos.sh`, `third_party/freedos/runtime/`).
6. Fallback path to `kernel.elf` remains tested.

## Source of Truth Documents
1. High-level roadmap (DOOM): `docs/roadmap-ciukios-doom.md`
2. High-level roadmap (Windows DOS-based): `docs/roadmap-windows-dosbased.md`
3. 16-bit execution layer design: `docs/opengem-016-design.md`
4. Compatibility baseline: `docs/int21-priority-a.md`
5. Phase history/kickoff: `docs/phase-0-kickoff.md`, `docs/phase-1.md`
6. FreeDOS licensing/integration policy: `docs/freedos-integration-policy.md`
7. FreeDOS symbiotic architecture: `docs/freedos-symbiotic-architecture.md`
8. Central durable documentation: `documentation.md`
9. Handoff index: `docs/handoffs/README.md`
10. Agent directives: `docs/agent-directives.md`

## Session Workflow (Required)
1. Read this file first.
2. Read `docs/agent-directives.md`.
3. Read `docs/collab/diario-di-bordo.md` before starting to avoid overlapping work.
4. Read the latest relevant handoff(s) in `docs/handoffs/`.
5. Create or switch to a dedicated task branch. Do not execute implementation work on `main`.
6. Execute scoped change.
7. Run relevant tests.
8. Write a new handoff for major multi-file or architectural changes.
9. Update `documentation.md` whenever the completed task changes stable project state, architecture, validation flow, or milestone status.
10. Update `docs/collab/diario-di-bordo.md` for every completed task; keep it local-only and untracked.
11. Update this file only if global direction/state changed.

## Merge Behavior (Required)
1. Implementation work must stay on a dedicated task branch.
2. If the user explicitly says `fai il merge`, treat that as approval to merge into `main`.
3. Before merging, verify whether conflicts exist.
4. If conflicts exist, inspect them, identify whether they come from other agents' work, and integrate all necessary changes instead of discarding one side.
5. Complete the final merge into `main` only after the conflict resolution is coherent.

## Versioning Cadence Rule (Required)
1. Current baseline version: `CiukiOS Alpha v0.8.9`.
2. Version bumps are user-controlled and must not happen unless the user explicitly requests one.
3. When a version bump is explicitly requested, update all of:
	- `README.md` (Current Version + Changelog)
	- `stage2/include/version.h`
	- relevant roadmap/status docs if user-visible scope changed
4. Keep changelog updates aligned with the user-requested version bump scope.

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
9
## Last Updated
2026-04-19
