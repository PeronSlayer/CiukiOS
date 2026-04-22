# Copilot Prompt - Desktop GUI Roadmap (5 Tasks)

You are assigned a GUI-focused roadmap for CiukiOS.

Read first:
1. `CLAUDE.md`
2. `docs/roadmap-ciukios-doom.md`
3. `docs/collab/copilot-gui-desktop-roadmap-2026-04-16.md`
4. latest `docs/handoffs/` entries

Execution model:
1. Do tasks in order from Task 1 to Task 5.
2. One branch per task (use branch names exactly from roadmap).
3. Keep changes scoped to listed files only.
4. Add one handoff file per task.

Hard constraints:
1. No loader/handoff ABI changes.
2. No brand references or emulation references.
3. Keep boot deterministic; no long/blocking render loops.
4. Do not revert unrelated Codex changes.

Required validation after each task:
- `make test-stage2`
- `make test-fallback`

Required validation before each merge request:
- `make check-int21-matrix`
- `make test-stage2`
- `make test-fallback`
- `make test-fat-compat`
- `make test-int21`
- `make test-freedos-pipeline`

Final report format per task:
1. Task ID + branch
2. Summary of implementation
3. Touched files
4. Test commands + result
5. Handoff path
