# Prompt For Assigned Agent - Third Agent CIUKEDIT Final Polish 001

Work on a dedicated branch only:

```bash
git fetch origin
git switch -c feature/third-agent-ciukedit-final-polish-001 origin/main
```

You are implementing a final polish pass on three visible CiukiOS product surfaces:

1. `CIUKEDIT.COM` must feel more definitively finished
2. visible debug/noise lines shown when launching `.COM` / `.EXE` programs from the shell must be cleaned up
3. the shell title/header must show the current CiukiOS version and use a white background with black text

Read first:
1. `CLAUDE.md`
2. `docs/agent-directives.md`
3. `docs/collab/diario-di-bordo.md`
4. `docs/copilot-task-third-agent-ciukedit-final-polish-001.md`
5. `docs/sr-edit-001.md`
6. `docs/copilot-task-sr-edit-001.md`
7. `com/ciukedit/ciukedit.c`
8. `stage2/src/shell.c`
9. `stage2/src/ui.c`
10. `stage2/src/stage2.c`
11. `stage2/include/version.h`
12. the most relevant recent handoff(s) under `docs/handoffs/`

Before starting implementation, confirm in `docs/collab/diario-di-bordo.md` that no other agent is already working on the same project area. If it is already occupied, stop and choose a different section instead of overlapping work.

Context:
CiukiOS now looks much more intentional than before, but a few visible surfaces still feel transitional rather than final-product-like. `CIUKEDIT.COM` still needs a stronger polish pass, normal `.COM` / `.EXE` launch still exposes too much debug-facing noise on the visible shell surface, and the shell title bar does not yet present the version/color treatment the user wants.

Your mission is to:
1. improve `CIUKEDIT.COM` so it feels like a more finished built-in user tool
2. remove or demote visible shell debug/noise lines that currently appear during ordinary `.COM` / `.EXE` launches
3. update the shell title/header to show the current CiukiOS version with a white bar and black text
4. keep the work tightly scoped to these user-visible polish surfaces rather than broad platform redesign

Hard requirements:
1. Do not bump the project version.
2. Do not commit on `main`.
3. Keep the work scoped to `CIUKEDIT` polish, visible launch-noise cleanup, and shell title/header styling.
4. Do not remove useful deterministic serial markers blindly; prefer moving validation/debug evidence to serial while cleaning the visible shell surface.
5. Do not redesign `CIUKEDIT` into a totally different editor architecture.
6. Preserve current DOS run-path behavior unless a change is directly required by this task.
7. Update `docs/collab/diario-di-bordo.md` at the end of the task, and do not add that file to Git.
8. If the user later says `fai il merge`, treat that as authorization to merge into `main`, but only after checking for conflicts and integrating all required changes from any conflicting side.

Implementation guidance:
1. Treat this as product-surface cleanup, not debug-system expansion.
2. Prefer cleaner user-visible output while preserving deterministic serial evidence for tests.
3. For `CIUKEDIT.COM`, focus on polish that materially improves real usage and perceived completeness.
4. For shell launch cleanup, remove or hide visible framebuffer text such as launch/runtime chatter that is not valuable to a normal user.
5. For the title bar, ensure the current version shown is sourced coherently from the actual runtime version state instead of being duplicated manually.

Validation required before handoff:
1. `make all`
2. `make test-stage2`
3. `bash scripts/test_ciukedit_smoke.sh` if still applicable, or an updated equivalent if your task changes the validation shape
4. any direct validation path you add for the title/header or launch-noise cleanup

Deliverables:
1. Working code on the dedicated branch
2. A more finished `CIUKEDIT.COM`
3. A cleaner visible shell launch surface for `.COM` / `.EXE`
4. A shell title/header that shows the current version with white background and black text
5. Any minimal docs/test updates needed
6. A handoff file named:
   - `docs/handoffs/YYYY-MM-DD-third-agent-ciukedit-final-polish-001.md`

In the final handoff, explicitly report:
1. final `CIUKEDIT.COM` user-facing behavior and what was improved
2. which visible launch/debug lines were removed, hidden, or redirected away from the shell surface
3. how the shell title/header now shows the current version and what colors/layout were chosen
4. tests run and their status
5. remaining rough edges before the shell/editor feel fully polished