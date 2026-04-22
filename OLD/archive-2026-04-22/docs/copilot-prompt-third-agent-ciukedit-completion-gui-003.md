# Prompt For Assigned Agent - Third Agent CIUKEDIT Completion + GUI 003

Work on a dedicated branch only:

```bash
git fetch origin
git switch -c feature/third-agent-ciukedit-completion-gui-003 origin/main
```

You are implementing the next substantial `CIUKEDIT.COM` task for Claude Opus.

Read first:
1. `CLAUDE.md`
2. `docs/agent-directives.md`
3. `docs/collab/diario-di-bordo.md`
4. `docs/copilot-task-third-agent-ciukedit-completion-gui-003.md`
5. `docs/copilot-task-third-agent-ciukedit-final-polish-001.md`
6. `docs/copilot-task-third-agent-ciukedit-header-polish-002.md`
7. `docs/sr-edit-001.md`
8. `com/ciukedit/ciukedit.c`
9. `stage2/src/shell.c`
10. `stage2/src/ui.c`
11. the latest relevant `CIUKEDIT` handoff(s) under `docs/handoffs/`

Before starting implementation, confirm in `docs/collab/diario-di-bordo.md` that no other agent is already working on the same project area. If it is already occupied, stop and choose a different section instead of overlapping work.

Context:
`CIUKEDIT.COM` already has a cleaner launch surface and better user-facing messages, but it is still not functionally complete. There is now a concrete user-reported bug: when opening a `.TXT` file created with `CIUKEDIT`, the file contents do not visibly appear in the editor surface even though the file is being loaded into the internal buffer. The user also wants the next step to materially improve `CIUKEDIT` functionality and GUI.

Your mission is to:
1. fix the root cause of the reopened-file content visibility bug
2. make loaded file contents actually appear in the visible editor surface after open
3. improve `CIUKEDIT` functionality so it feels less like an append-only utility and more like a usable built-in editor
4. improve the GUI/editor chrome within the current CiukiOS text/video model
5. keep the work coherent, practical, and compatible with the current DOS/text-mode runtime

Hard requirements:
1. Do not bump the project version.
2. Do not commit on `main`.
3. Fix the loaded-file visibility problem at the root cause.
4. Do not regress the current startup cleanliness, top bar, save/open/quit behavior, or deterministic validation markers without strong justification.
5. Do not broaden into a generic desktop/window manager redesign.
6. Stay inside the current CiukiOS runtime and text/video constraints.
7. Update `docs/collab/diario-di-bordo.md` at the end of the task, and do not add that file to Git.
8. If the user later says `fai il merge`, treat that as authorization to merge into `main`, but only after checking for conflicts and integrating all required changes from any conflicting side.

Implementation guidance:
1. First reproduce and understand why loaded content is not visible even though `load_file()` fills the buffer.
2. Prefer a real editor-surface model over one-off prints or temporary dumps.
3. Ensure the visible surface reflects the current buffer state after open, edit, delete, and save operations.
4. If you introduce viewport/cursor/status concepts, keep them minimal, robust, and testable.
5. Treat this as the next real editor milestone, not as another purely cosmetic pass.

Validation required before handoff:
1. `make all`
2. `bash scripts/test_ciukedit_smoke.sh` if still applicable, or an updated equivalent if needed
3. `CIUKIOS_SKIP_BUILD=1 TIMEOUT_SECONDS=60 make test-stage2` if still valid on this host
4. add a focused validation path proving that a file saved by `CIUKEDIT` is reopened with its content visibly rendered

Deliverables:
1. Working code on the dedicated branch
2. A more complete `CIUKEDIT.COM`
3. A stronger visible editor GUI/surface
4. A root-cause fix for the reopened-file content visibility bug
5. Any minimal validation/doc updates needed
6. A handoff file named:
   - `docs/handoffs/YYYY-MM-DD-third-agent-ciukedit-completion-gui-003.md`

In the final handoff, explicitly report:
1. the root cause of the “saved file opens but appears empty” bug
2. how the bug was fixed and how you verified it
3. what GUI/editor-surface improvements were added
4. what functionality was completed or materially improved
5. tests run and their status
6. any remaining rough edges before `CIUKEDIT` is considered substantially complete