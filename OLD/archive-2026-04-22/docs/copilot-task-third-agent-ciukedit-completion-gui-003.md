# Copilot Task Pack - Third Agent CIUKEDIT Completion + GUI 003

Baseline: CiukiOS Alpha v0.8.7. Do NOT touch `main` directly.

## Goal

Deliver the next substantial evolution of `CIUKEDIT.COM` from a polished line-oriented tool into a more complete first-party editor surface.

This task has two equally important objectives:

1. fix the real usability bug where reopening a `.TXT` previously saved with `CIUKEDIT` does not visibly show the loaded content in the editor surface
2. improve `CIUKEDIT` functionality and GUI so it feels materially closer to a usable built-in text editor rather than a shell-like append-only utility

## Context

Recent work already improved:

1. launch cleanliness
2. top command bar rendering
3. serial-vs-visible output separation
4. basic save/open markers and friendlier messages

However, the current editor still has a major usability gap:

1. `load_file()` populates the internal line buffer, but the loaded content is not actually rendered into the visible editing surface on open, so a text file created with `CIUKEDIT` can appear empty to the user unless they explicitly list lines

That means the current behavior is not just cosmetically incomplete; it is functionally misleading.

## Read First

1. `CLAUDE.md`
2. `docs/agent-directives.md`
3. `docs/collab/diario-di-bordo.md`
4. `docs/copilot-task-third-agent-ciukedit-final-polish-001.md`
5. `docs/copilot-task-third-agent-ciukedit-header-polish-002.md`
6. `docs/sr-edit-001.md`
7. `com/ciukedit/ciukedit.c`
8. `stage2/src/shell.c`
9. `stage2/src/ui.c`
10. any latest relevant `CIUKEDIT` handoff under `docs/handoffs/`

Before implementation, confirm in `docs/collab/diario-di-bordo.md` that no other agent is already working on this same area. If occupied, stop and choose a different section instead of overlapping.

## Scope

Advance `CIUKEDIT.COM` in a focused but meaningful way.

Required scope:

1. root-cause fix for the loaded-file-visible-content bug
2. a real visible editing surface that renders existing buffer contents after file open
3. stronger editor GUI/chrome beyond the current minimal header
4. improved editing interaction so the tool feels materially more complete in normal use

Examples of acceptable directions, if they fit the current architecture:

1. render the text buffer in the main content area automatically after open
2. add a status/footer row or richer header information
3. introduce a viewport/cursor model that lets the user see where they are in the buffer
4. improve line insertion/editing/deletion flow so the visible editor surface and the internal buffer stay in sync
5. add small but meaningful editing/navigation controls if compatible with the existing DOS/text-mode environment

Do NOT broaden this into a totally different app architecture or a speculative desktop/windowing rewrite. Stay inside the current CiukiOS text/video/runtime model.

## Hard Requirements

1. Do not bump the project version.
2. Do not commit on `main`.
3. Fix the loaded-file visibility bug at the root cause, not with a fake success message.
4. Keep deterministic serial markers useful for validation unless there is a strong reason to evolve them.
5. Do not regress current open/save/quit behavior.
6. Do not break `CIUKEDIT` startup cleanliness introduced by the previous micro-task.
7. Update `docs/collab/diario-di-bordo.md` when the task is done, and do not add that file to Git.
8. Add a handoff file: `docs/handoffs/YYYY-MM-DD-third-agent-ciukedit-completion-gui-003.md`

## Validation Required

1. `make all`
2. `bash scripts/test_ciukedit_smoke.sh` if still applicable, or an updated equivalent if the editor interaction model needs a stronger gate
3. `CIUKIOS_SKIP_BUILD=1 TIMEOUT_SECONDS=60 make test-stage2` if still valid on this host
4. add a focused validation that proves a file saved by `CIUKEDIT` is later reopened with visible content correctly rendered

If host runtime capture is still limited, keep a deterministic static/source fallback and clearly document the runtime limitation.

## Deliverables

1. Working code on a dedicated branch
2. A more functionally complete `CIUKEDIT.COM`
3. A stronger editor GUI/surface
4. A real fix for the reopened-file content visibility bug
5. Any minimal docs/test updates needed
6. A handoff file documenting scope, decisions, tests, risks, and next step

## Final Handoff Must Explicitly Report

1. what the root cause of the “saved text file opens but content does not appear” bug was
2. how that bug was fixed and how it was validated
3. what new GUI/editor-surface capabilities were added
4. what editing/navigation functionality materially improved
5. tests run and their status
6. remaining rough edges before `CIUKEDIT` feels genuinely complete