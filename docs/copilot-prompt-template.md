# Prompt Template For Assigned Agent

Work on a dedicated branch only:

```bash
git fetch origin
git switch -c feature/<task-branch-name> origin/main
```

You are implementing `<task summary>` for CiukiOS.

Read first:
1. `CLAUDE.md`
2. `docs/agent-directives.md`
3. `docs/collab/diario-di-bordo.md`
4. `<task specification doc>`
5. `<relevant roadmap doc>`
6. `<primary source file 1>`
7. `<primary source file 2>`

Before starting implementation, confirm in `docs/collab/diario-di-bordo.md` that no other agent is already working on the same project area. If it is already occupied, stop and choose a different section instead of overlapping work.

Context:
`<brief current-state summary>`

Your mission is to:
1. `<goal 1>`
2. `<goal 2>`
3. `<goal 3>`

Hard requirements:
1. Do not bump the project version.
2. Do not commit on `main`.
3. Keep the work scoped to `<scope>`.
4. Preserve deterministic logs and current runtime behavior unless the task explicitly requires otherwise.
5. Update `docs/collab/diario-di-bordo.md` at the end of the task, and do not add that file to Git.
6. If the user later says `fai il merge`, treat that as authorization to merge into `main`, but only after checking for conflicts and integrating all required changes from any conflicting side.

Implementation guidance:
1. `<guidance 1>`
2. `<guidance 2>`
3. `<guidance 3>`

Validation required before handoff:
1. `<build/test command 1>`
2. `<build/test command 2>`
3. `<build/test command 3>`

Deliverables:
1. Working code on the dedicated branch
2. Any minimal docs/test updates needed
3. A handoff file named:
   - `docs/handoffs/YYYY-MM-DD-<topic>.md`

In the final handoff, explicitly report:
1. `<result item 1>`
2. `<result item 2>`
3. `<result item 3>`
4. tests run and their status
5. remaining risks or gaps