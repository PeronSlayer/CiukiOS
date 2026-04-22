# Copilot Task Pack - Third Agent CIUKEDIT Header Polish 002

Baseline: CiukiOS Alpha v0.8.7. Do NOT touch `main` directly.

## Goal

Deliver one small, tightly scoped follow-up improvement to `CIUKEDIT.COM`:

1. when `CIUKEDIT.COM` starts, the editor surface must feel immediately clean and intentional
2. the screen should be cleared before the editing session appears
3. a white top bar should be visible at the top
4. that top bar should show the basic `CIUKEDIT` commands
5. below the top bar, the writing area should begin immediately with the cursor ready for input

This is a micro-polish task, not a broader redesign.

## Read First

1. `CLAUDE.md`
2. `docs/agent-directives.md`
3. `docs/collab/diario-di-bordo.md`
4. `docs/copilot-task-third-agent-ciukedit-final-polish-001.md`
5. `com/ciukedit/ciukedit.c`
6. any new handoff produced by the previous `CIUKEDIT` final polish task

Before implementation, confirm in `docs/collab/diario-di-bordo.md` that no other agent is already working on this same area. If occupied, stop and choose a different section instead of overlapping.

## Scope

Only improve the initial visible editor layout when `CIUKEDIT.COM` launches.

The intended result is:

1. clear screen on entry
2. white bar/header at top
3. black text inside that bar with the core commands/help shortcuts
4. editor prompt/cursor starts below that bar, in a clean writing area

Keep it compatible with the current line-oriented architecture. Do not turn `CIUKEDIT` into a full-screen modal editor.

## Hard Requirements

1. Do not bump the project version.
2. Do not commit on `main`.
3. Keep the scope limited to `CIUKEDIT.COM` startup layout polish.
4. Do not regress save/open/quit behavior.
5. Do not remove deterministic serial markers that the current validation still depends on.
6. Update `docs/collab/diario-di-bordo.md` when the task is done, and do not add that file to Git.
7. Add a handoff file: `docs/handoffs/YYYY-MM-DD-third-agent-ciukedit-header-polish-002.md`

## Validation Required

1. `make all`
2. `bash scripts/test_ciukedit_smoke.sh` if still applicable
3. any direct validation you add for the new startup header/layout behavior

## Final Handoff Must Explicitly Report

1. how the startup screen/layout of `CIUKEDIT.COM` changed
2. which commands are shown in the top bar
3. where the input area/cursor begins after launch
4. tests run and their status
5. any remaining rough edges in the editor surface