# Copilot Claude Task Pack - SR-VIDEO-001 Expansion v2

## Mandatory Branch Isolation
Claude must work only on a dedicated branch and must not touch `main` directly.

```bash
git fetch origin
git switch -c feature/copilot-claude-sr-video-001-v2 origin/main
```

No direct commits on `main`. No force-push on shared branches.

## Mission
Expand `SR-VIDEO-001` from first-pass driver into a more capable and deterministic subsystem suitable for DOS workloads and GUI layering.

## Scope (5 heavy tasks)

### V1) Layered Text/UI Plane over GOP Backbuffer
1. Introduce a dedicated text overlay plane that can be composited on top of gfx backbuffer without full redraw.
2. Add explicit APIs:
- `video_overlay_mark_dirty(...)`
- `video_overlay_present_dirty()`
- `video_overlay_clear_region(...)`
3. Ensure shell cursor/text remain readable during continuous gfx updates.

Deliverable marker:
- serial: `[video] overlay plane active`

### V2) Robust Present Scheduler + Frame Pacing
1. Add frame pacing policy (tick-based) to prevent over-present and reduce jitter.
2. Keep deterministic behavior under QEMU (no busy-loop rendering storms).
3. Add runtime counters for `present_full`, `present_dirty`, dropped/coalesced presents.

Deliverable marker:
- serial: `[video] pacing stable present_full=... present_dirty=...`

### V3) Resolution-Independent Layout Metrics
1. Replace hardcoded pixel constants in desktop layout with metric helpers based on current mode.
2. Enforce alignment grid (panel spacing/margins/line heights) for 800x600, 1024x768, 1280x800, 1920x1080.
3. Add clipping guards for every panel draw path.

Deliverable marker:
- serial: `[ui] layout metrics v3 active`

### V4) Glyph Pipeline Upgrade (Readability)
1. Add 2 font scales (small/normal) with explicit baseline and line-height management.
2. Auto-select readable default by resolution class.
3. Ensure no shell text truncation/regression when switching mode.

Deliverable marker:
- serial: `[video] font profile=<name> cell=<w>x<h>`

### V5) Regression Gates for Video/UI Stability
1. Add `scripts/test_video_ui_regression_v2.sh`.
2. Validate presence of new markers and absence of regressions (`panic`, `#UD`, layout markers missing).
3. Add Make target: `test-video-ui-v2`.

## Constraints
1. Keep current commands functional: `desktop`, `vmode`, `vres`, shell core commands.
2. Do not remove existing markers used by current tests.
3. Keep code freestanding-safe and deterministic.
4. Avoid large ABI breaks; if unavoidable, document exact handoff changes.

## Validation
Before handoff, run:
1. `make all`
2. `make test-stage2`
3. `make test-video-mode`
4. `make test-video-1024`
5. `make test-gui-desktop`

## Final Handoff
Create:
- `docs/handoffs/YYYY-MM-DD-copilot-claude-sr-video-001-v2.md`

Include:
1. changed files
2. markers added
3. tests executed + outcomes
4. known limits
5. next 5 suggested tasks
