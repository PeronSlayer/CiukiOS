# Copilot Prompt - GUI Cycle (2026-04-16)

You are working on CiukiOS as GUI-focused agent for this cycle.

Read first:
1. `CLAUDE.md`
2. `docs/roadmap-ciukios-doom.md`
3. `docs/collab/parallel-next-tasks-2026-04-16-gui-plus-roadmap.md`
4. latest files in `docs/handoffs/`

Execute these tasks in order, each on its own branch:

## Task G1 - UI Primitives Module
Branch:
- `feature/copilot-gui-ui-primitives`

Implement:
1. Add `stage2/include/ui.h` and `stage2/src/ui.c` with reusable helpers:
   - top bar draw
   - centered label draw
   - progress bar draw
   - panel/frame draw helper
2. Refactor current stage2 boot rendering in `stage2/src/stage2.c` to call these helpers.
3. Keep behavior equivalent (no functional regressions).

Tests required:
- `make test-stage2`
- `make test-fallback`

## Task G2 - Boot HUD Status Overlay
Branch:
- `feature/copilot-gui-boot-hud`

Implement:
1. Add compact boot HUD in graphical mode with:
   - CiukiOS label
   - version string
   - splash mode (gfx/ascii)
   - progress percentage
2. Add serial marker once when HUD becomes active:
   - `[ ui ] boot hud active`
3. Optional: update `scripts/test_stage2_boot.sh` to assert this marker if stable.

Tests required:
- `make test-stage2`
- `make test-fallback`
- `make test-fat-compat`

Hard constraints:
1. Do not modify loader ABI/handoff structures.
2. Do not revert unrelated work.
3. Keep changes deterministic and boot-safe.
4. Add one handoff per task under `docs/handoffs/`.

When done, provide:
1. summary
2. touched files
3. tests + result
4. handoff paths
