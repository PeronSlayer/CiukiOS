# HANDOFF - DOOM target roadmap + Claude sync readme + FreeDOS policy

## Context
User requested a full roadmap toward running DOS DOOM on CiukiOS, plus a dedicated shared readme for Claude Code and Codex, and clarification on integrating DOS system files via FreeDOS (GPL ecosystem).

## What Changed
1. Added an execution-focused roadmap to the game milestone:
   - `docs/roadmap-ciukios-doom.md`
2. Added FreeDOS integration and licensing policy document:
   - `docs/freedos-integration-policy.md`
3. Added project-level collaboration readme for Claude/Codex alignment:
   - `CLAUDE.md`
4. Updated top-level README with current direction and doc map:
   - `README.md`
5. Added cross-link note in existing DOS 6.2 roadmap:
   - `docs/roadmap-dos62-compat.md`

## Decisions
1. Keep roadmap split:
   - `roadmap-dos62-compat.md` as broad compatibility blueprint
   - `roadmap-ciukios-doom.md` as concrete execution path to near-term north star
2. Treat FreeDOS as preferred redistributable base, with per-component license verification.
3. Keep Microsoft DOS assets out of default public distribution path.

## Validation
1. Documentation-only change set.
2. No code/runtime tests executed in this step.

## Risks
1. License assumptions can still be wrong per individual FreeDOS package if provenance is not tracked.
2. DOOM protected-mode/extender milestone may require additional design pivots once implementation starts.

## Immediate Next Step
1. Start M1 of `docs/roadmap-ciukios-doom.md`: true DOS `.COM`/`.EXE MZ` execution core with PSP + relocation tests.
