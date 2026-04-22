# Prompt For Assigned Agent - Third Agent CIUKEDIT Header Polish 002

Work on a dedicated branch only:

```bash
git fetch origin
git switch -c feature/third-agent-ciukedit-header-polish-002 origin/main
```

You are implementing one small focused follow-up improvement to `CIUKEDIT.COM`.

Read first:
1. `CLAUDE.md`
2. `docs/agent-directives.md`
3. `docs/collab/diario-di-bordo.md`
4. `docs/copilot-task-third-agent-ciukedit-header-polish-002.md`
5. `docs/copilot-task-third-agent-ciukedit-final-polish-001.md`
6. `com/ciukedit/ciukedit.c`
7. the latest `CIUKEDIT` handoff under `docs/handoffs/`

Before starting implementation, confirm in `docs/collab/diario-di-bordo.md` that no other agent is already working on the same project area. If it is already occupied, stop and choose a different section instead of overlapping work.

Context:
The previous `CIUKEDIT` polish pass improved the editor significantly, but the user wants one more visual cleanup: when the editor starts, it should immediately feel like a clean dedicated editing surface instead of a generic text session.

Your mission is to:
1. clear the screen when `CIUKEDIT.COM` launches
2. render a white top bar/header at the top of the editor
3. show the basic `CIUKEDIT` commands in that top bar using black text
4. make the actual writing area start below the bar with the cursor ready to type
5. keep the implementation tightly scoped to startup/editor layout polish

Hard requirements:
1. Do not bump the project version.
2. Do not commit on `main`.
3. Keep the work scoped to `CIUKEDIT.COM` startup/header polish only.
4. Do not redesign the editor architecture or broaden into a full-screen modal editor.
5. Do not regress current open/save/quit behavior.
6. Preserve any deterministic serial markers still needed by the current validation flow.
7. Update `docs/collab/diario-di-bordo.md` at the end of the task, and do not add that file to Git.
8. If the user later says `fai il merge`, treat that as authorization to merge into `main`, but only after checking for conflicts and integrating all required changes from any conflicting side.

Implementation guidance:
1. Prefer a very clean launch surface: no leftover shell clutter.
2. Keep the top bar compact and readable.
3. The commands shown in the header should be the core editor actions only.
4. Keep the typing area immediately usable after startup.
5. Stay consistent with the current CiukiOS text/video constraints instead of inventing a new UI framework.

Validation required before handoff:
1. `make all`
2. `bash scripts/test_ciukedit_smoke.sh` if still applicable, or an updated equivalent if your layout change requires it
3. any direct validation you add for the startup header/layout behavior

Deliverables:
1. Working code on the dedicated branch
2. A cleaner `CIUKEDIT.COM` startup surface with a white top bar
3. Black command text in that bar
4. Cursor/input area starting below the bar
5. Any minimal validation/doc updates needed
6. A handoff file named:
   - `docs/handoffs/YYYY-MM-DD-third-agent-ciukedit-header-polish-002.md`

In the final handoff, explicitly report:
1. what the `CIUKEDIT` startup screen now looks like
2. which commands are shown in the header bar
3. where the input area begins and how the cursor is positioned
4. tests run and their status
5. any remaining small rough edges