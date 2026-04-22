# Prompt For Assigned Agent - SR-VIDEO-003

Work on a dedicated branch only:

```bash
git fetch origin
git switch -c feature/copilot-codex-sr-video-003 origin/main
```

You are implementing the first real VGA mode `0x13` graphics checkpoint for CiukiOS.

Read first:
1. `CLAUDE.md`
2. `docs/agent-directives.md`
3. `docs/collab/diario-di-bordo.md`
4. `docs/copilot-task-codex-sr-video-003.md`
5. `docs/roadmap-ciukios-doom.md`
6. `docs/subroadmap-sr-video-002.md`
7. `stage2/include/gfx_modes.h`
8. `stage2/src/gfx_modes.c`
9. `stage2/src/stage2.c`
10. `stage2/src/shell.c`
11. `com/dosmode13/dosmode13.c`
12. `scripts/test_vga13_baseline.sh`

Before starting implementation, confirm in `docs/collab/diario-di-bordo.md` that no other agent is already working on the same project area. If it is already occupied, stop and choose a different section instead of overlapping work.

Context:
CiukiOS already has a VGA mode `0x13` compatibility scaffold, an indexed 320x200 plane, palette helpers, a present path, and a sample COM program (`DOSMODE13.COM`). But the current gate is still mostly static and the roadmap still treats the real draw/render checkpoint as pending.

Your mission is to turn that into a stronger, deterministic runtime milestone.

What you must do:
1. Strengthen the mode `0x13` path so the important transitions are observable through deterministic markers.
2. Upgrade `DOSMODE13.COM` into a real frame-checkpoint sample, not just a bare API poke.
3. Replace or extend the current VGA baseline gate so it validates a runtime draw/present path when possible, with a static fallback only when needed.
4. Document exactly what the new checkpoint proves and what still remains deferred before DOOM.

Hard requirements:
1. Do not bump the project version.
2. Do not commit on `main`.
3. Keep the work narrowly focused on the first real mode `0x13` checkpoint.
4. Do not broaden into WAD parsing, audio, or generic DOS extender redesign.
5. Preserve deterministic logs and current shell/runtime behavior.
6. Update `docs/collab/diario-di-bordo.md` at the end of the task, and do not add that file to Git.

Implementation guidance:
1. Reuse the existing `gfx_modes` plane/palette/present path instead of inventing a second graphics subsystem.
2. If the current runtime path has silent failures, surface them through explicit serial markers.
3. Prefer a sample frame with multiple distinct visual regions so the checkpoint is more meaningful than a flat color fill.
4. Reuse the existing QEMU gate style already used elsewhere in the repo if you need a runtime+fallback test structure.
5. Keep text-mode return behavior clean if the ABI already supports it; otherwise document the limitation rather than guessing.

Validation required before handoff:
1. `make all`
2. `make test-stage2`
3. `make test-vga13-baseline`
4. any direct runtime validation path you add for `DOSMODE13.COM`

Deliverables:
1. Working code on the dedicated branch
2. Updated VGA validation gate
3. Any minimal docs updates needed
4. A handoff file named:
   - `docs/handoffs/YYYY-MM-DD-copilot-codex-sr-video-003.md`

In the final handoff, explicitly report:
1. final sample behavior in mode `0x13`
2. emitted markers and what they prove
3. gate behavior in runtime and fallback modes
4. tests run and their status
5. remaining gaps before the DOOM first-frame/menu milestone