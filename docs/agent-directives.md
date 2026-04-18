# Agent Directives

These directives are mandatory for GitHub Copilot, Codex, Claude Code, and any delegated agent working on CiukiOS.

## Mandatory Rules
1. Every task must be executed on a dedicated branch. `main` must never be used as the working branch for implementation work.
2. Every completed task must update the shared local logbook at `docs/collab/diario-di-bordo.md`.
3. The logbook is local-only collaboration state and must remain untracked by Git. Do not add or push it to GitHub.
4. Version bumps are user-controlled only. Never bump the project version unless the user explicitly asks for it.
5. Before assigning a task to yourself or another agent, verify whether another agent is already working on that project area. If so, choose a different section.

## Required Agent Workflow
1. Read `CLAUDE.md`.
2. Read this file.
3. Read `docs/collab/diario-di-bordo.md` to understand active work and avoid overlap.
4. Create or switch to a dedicated branch for the task.
5. Execute the scoped work.
6. Update `docs/collab/diario-di-bordo.md` when the task is completed or handed off.

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