# Prompt For Assigned Agent - OT-DEMO-001

Work on a dedicated branch only:

```bash
git fetch origin
git switch -c feature/third-agent-ot-demo-001 origin/main
```

You are preparing a demo-oriented polish pass for CiukiOS.

Read first:
1. `CLAUDE.md`
2. `docs/agent-directives.md`
3. `docs/collab/diario-di-bordo.md`
4. `docs/copilot-task-third-agent-ot-demo-001.md`
5. `docs/roadmap-ciukios-doom.md`
6. `stage2/src/shell.c`
7. `stage2/src/stage2.c`
8. `stage2/src/gfx_modes.c`
9. `stage2/include/gfx_modes.h`
10. relevant graphics demo/sample programs under `com/`

Before starting implementation, confirm in `docs/collab/diario-di-bordo.md` that no other agent is already working on the same project area. If it is already occupied, stop and choose a different section instead of overlapping work.

Context:
CiukiOS is now far enough along that a short video demo should look intentional instead of like a bring-up environment. The shell still exposes too much test/internal surface for a polished demo, and there is not yet a dedicated short-form animated graphics showcase meant specifically for recording the current state of the project.

Your mission is to:
1. make the shell look more final-product-oriented for a short recorded demo
2. reduce or hide low-value internal/test commands from the visible shell surface
3. add one graphical demo program that draws a deterministic animated scene for roughly 30 seconds
4. keep the work tightly scoped to demo presentation polish rather than broad platform redesign

Hard requirements:
1. Do not bump the project version.
2. Do not commit on `main`.
3. Keep the work scoped to demo-oriented shell cleanup and one real-time graphics showcase program.
4. Do not broaden into WAD parsing, audio, or generic DOS extender redesign.
5. Preserve deterministic logs and current runtime behavior unless the task explicitly needs a minimal presentation adjustment.
6. Update `docs/collab/diario-di-bordo.md` at the end of the task, and do not add that file to Git.
7. If the user later says `fai il merge`, treat that as authorization to merge into `main`, but only after checking for conflicts and integrating all required changes from any conflicting side.

Implementation guidance:
1. Prefer hiding/demoting internal-only commands from primary help/discoverability over deleting useful infrastructure blindly.
2. Make the shell help and visible command surface feel curated and intentional.
3. Reuse the existing graphics/runtime path instead of inventing a second subsystem.
4. The graphics demo should be deterministic, visually readable in a video, and not depend on external assets.
5. Emit clear runtime markers for the graphics demo so its main phases are greppable when practical.

Validation required before handoff:
1. `make all`
2. `make test-stage2`
3. any direct runtime validation path added for the graphics demo
4. any updated shell/help validation path if touched

Deliverables:
1. Working code on the dedicated branch
2. A cleaner demo-oriented shell surface
3. One dedicated real-time graphics showcase program
4. Any minimal docs/test updates needed
5. A handoff file named:
   - `docs/handoffs/YYYY-MM-DD-third-agent-ot-demo-001.md`

In the final handoff, explicitly report:
1. final shell surface and which commands were removed, hidden, or demoted
2. final graphics demo behavior over its ~30 second runtime
3. emitted markers and what they prove
4. tests run and their status
5. remaining rough edges before the system is fully ready for a polished short video demo