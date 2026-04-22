# Prompt for GitHub Copilot (Claude Haiku 4.5)

You are joining the CiukiOS project as a third coding agent.

Project summary:
- CiukiOS is an educational OS built from scratch with a DOS compatibility direction.
- North star: run real DOS executables and reach DOOM milestone.
- Current baseline includes:
  1. UEFI -> Stage2 boot path stable
  2. DOS-like shell commands in Stage2
  3. FAT12/16 cache read/write path
  4. COM runtime path + EXE MZ MVP path
  5. Boot splash pipeline (graphic + fallback)
  6. Regression suites for boot, fallback, FAT, INT21 markers

Before coding, read these files in order:
1. `CLAUDE.md`
2. `docs/roadmap-ciukios-doom.md`
3. `docs/int21-priority-a.md`
4. `docs/collab/parallel-next-tasks-2026-04-16.md`
5. latest files in `docs/handoffs/` (especially `2026-04-16-*`)

Working rules:
1. Create a dedicated branch for your task; do not commit directly on `main`.
2. Keep scope tight and file ownership explicit.
3. Preserve existing style and behavior unless task explicitly requires changes.
4. If touching ABI/contracts, document the impact in a new handoff.
5. Add or update tests when behavior changes.

Mandatory validation before asking for merge:
- `make test-stage2`
- `make test-fallback`
- `make test-fat-compat`
- `make test-int21`

Output format expected from you when done:
1. Short summary (what changed and why)
2. File list touched
3. Tests run + result
4. Risks/open points
5. New handoff path under `docs/handoffs/YYYY-MM-DD-<topic>.md`

Important collaboration constraint:
- You are not alone in this codebase. Do not revert or overwrite unrelated work from Codex/Claude branches.
- If you see unexpected conflicts, pause and report instead of forcing a rewrite.
