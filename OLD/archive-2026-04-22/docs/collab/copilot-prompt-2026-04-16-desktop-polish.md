# Copilot Prompt - Desktop Polish Cycle (5 Tasks)

You are assigned a desktop quality cycle for CiukiOS.

Read first:
1. `CLAUDE.md`
2. `docs/roadmap-ciukios-doom.md`
3. `docs/collab/copilot-desktop-polish-roadmap-2026-04-16.md`
4. latest entries in `docs/handoffs/`

Execution model:
1. Execute tasks in strict order: D1 -> D5.
2. One branch per task (use branch names exactly as defined).
3. Keep change scope inside each task's owned files.
4. Add one handoff file per task under `docs/handoffs/`.

Hard constraints:
1. No loader ABI/handoff changes.
2. No random visual artifacts; prioritize readability over visual complexity.
3. Keep boot deterministic, no long/blocking animation loops.
4. Do not revert unrelated Codex/Claude work.

Required tests after each task:
- `make test-stage2`
- `make test-fallback`

Required tests before each merge request:
- `make check-int21-matrix`
- `make test-stage2`
- `make test-int21`
- `make test-fallback`
- `make test-fat-compat`

Final report format per task:
1. Task ID + branch
2. Summary of implementation
3. Touched files
4. Test commands + result
5. Handoff path
