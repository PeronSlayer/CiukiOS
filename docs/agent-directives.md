# Agent Directives

These directives are mandatory for GitHub Copilot, Codex, Claude Code, and any delegated agent working on CiukiOS.

## Mandatory Rules
1. Every task must be executed on a dedicated branch. `main` must never be used as the working branch for implementation work.
2. Every completed task must update the shared local logbook at `docs/collab/diario-di-bordo.md`.
3. The logbook is local-only collaboration state and must remain untracked by Git. Do not add or push it to GitHub.
4. Version bumps are user-controlled only. Never bump the project version unless the user explicitly asks for it.
5. Before assigning a task to yourself or another agent, verify whether another agent is already working on that project area. If so, choose a different section.
6. When the user explicitly says `fai il merge`, interpret that as authorization to merge into `main`.
7. Before merging into `main`, you must verify whether conflicts exist.
8. If conflicts exist, inspect which files conflict, determine whether they come from other agents' changes, and integrate all required changes instead of dropping any side.
9. Only after conflict resolution is complete may the final merge into `main` be completed.

## Required Agent Workflow
1. Read `CLAUDE.md`.
2. Read this file.
3. Read `docs/collab/diario-di-bordo.md` to understand active work and avoid overlap.
4. Create or switch to a dedicated branch for the task.
5. Execute the scoped work.
6. Update `docs/collab/diario-di-bordo.md` when the task is completed or handed off.

## Merge Rule
1. Normal implementation work must stay on a dedicated branch, not on `main`.
2. If the user explicitly says `fai il merge`, merge the completed work into `main` directly.
3. Before the merge, verify whether merge conflicts exist.
4. If conflicts exist, inspect the conflicting files and identify whether they contain other agents' modifications.
5. Resolve conflicts by preserving and integrating all required changes from both sides whenever technically possible.
6. Complete the final merge into `main` only after the conflict resolution result is coherent and validated.

## Prompt Authoring Rule
Every new prompt written for another agent must explicitly tell the agent to read:
1. `CLAUDE.md`
2. `docs/agent-directives.md`
3. `docs/collab/diario-di-bordo.md`

## Logbook Update Minimum
Each logbook entry should record:
1. Date
2. Agent or branch name
3. Area of the project being changed
4. Status (`planned`, `in progress`, `blocked`, `done`)
5. Short summary of what changed or what remains