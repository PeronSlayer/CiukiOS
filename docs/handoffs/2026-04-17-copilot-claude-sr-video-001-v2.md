# Handoff: SR-VIDEO-001 Expansion v2

**Date:** 2026-04-17  
**Branch:** `feature/copilot-claude-sr-video-001-v2`  
**Task pack:** `docs/copilot-task-claude-sr-video-001-v2.md`

## Context and Goal

Expand SR-VIDEO-001 from first-pass driver into a more capable and deterministic subsystem suitable for DOS workloads and GUI layering. Implement 5 heavy tasks: overlay plane, frame pacing, resolution-independent layout metrics, glyph pipeline upgrade, and regression gates.

## Files Changed

| File | Action | Description |
|------|--------|-------------|
| `stage2/include/video.h` | Modified | Added overlay (V1), pacing (V2), font profile (V4) API declarations |
| `stage2/src/video.c` | Modified | Implemented overlay plane, frame pacing scheduler, font profile auto-selection |
| `stage2/include/ui.h` | Modified | Added `ui_metrics_t` struct and `ui_metrics_apply()` API (V3) |
| `stage2/src/ui.c` | Modified | Implemented resolution-independent layout metrics, clipping guards |
| `stage2/src/shell.c` | Modified | Added `video_pacing_report()` call on desktop session exit |
| `scripts/test_video_ui_regression_v2.sh` | New | 14-gate regression script for V1-V4 validation |
| `Makefile` | Modified | Added `test-video-ui-v2` target |

## Serial Markers Added

| Marker | Task | Emitted by |
|--------|------|------------|
| `[video] overlay plane active` | V1 | `video_overlay_init()` in video.c |
| `[video] pacing stable present_full=N present_dirty=N coalesced=N` | V2 | `video_pacing_report()` in video.c |
| `[ui] layout metrics v3 active` | V3 | `ui_metrics_apply()` in ui.c |
| `[video] font profile=<name> cell=<W>x<H>` | V4 | `video_select_font_profile()` in video.c |

## Tests Executed and Outcomes

| Test | Type | Result |
|------|------|--------|
| `make all` | Build | **PASS** - Clean compilation, no warnings |
| `make test-video-1024` | Static | **PASS** - 1024x768 baseline compat gate |
| `make test-video-backbuf` | Static | **PASS** - Backbuffer budget consistency |
| `make test-video-ui-v2` | Static | **PASS** - 14/14 gates (V1-V4 markers + regression checks) |
| `make test-stage2` | QEMU | INFRA - Serial capture timeout (known infra limitation) |
| `make test-video-mode` | QEMU | INFRA - Requires QEMU serial capture |
| `make test-gui-desktop` | QEMU | INFRA - Requires QEMU serial capture |

## Decisions Made

1. **Overlay plane shares backbuffer:** The overlay doesn't use a separate buffer; it tracks a distinct dirty region over the main backbuffer. This avoids additional BSS cost while allowing independent text/UI flushes.

2. **Pacing at 30 FPS default:** `video_pacing_init(30U)` — reasonable for desktop workloads under QEMU (PIT at 100Hz). Prevents busy-loop rendering storms.

3. **Font profiles (V4):** Two profiles — `small` (1x1 scale, ≤800x600) and `normal` (2x2 scale, >800x600). Auto-selected at `video_init()` based on resolution class.

4. **Layout metrics (V3):** Scale factor based on resolution class (1x for ≤1024, 2x for ≥1280, 3x for ≥1920). Grid remains 8px base. Clipping guards added to `ui_compute_layout()`.

5. **Pacing counters integrate with existing present path:** `video_present()` and `video_present_dirty()` now respect pacing interval and increment `g_present_full_count`/`g_present_dirty_count`/`g_present_coalesced`.

## Known Limits

1. QEMU-based tests (test-stage2, test-video-mode, test-gui-desktop) not validated due to serial capture infrastructure limitation in this environment.
2. Overlay plane uses single bounding-box tracking (not per-cell). Fine for current shell/text overlay patterns but may need refinement for complex multi-region overlays.
3. Font profile only has 2 tiers (small/normal). A third "large" tier for 4K displays could be added when hardware support warrants it.
4. Layout metrics are computed per `ui_compute_layout()` call but not yet consumed by all draw paths — some panels still use compile-time constants from `ui.h`.

## Next 5 Suggested Tasks

1. **Integrate metrics into window renderer:** Replace remaining hardcoded `UI_PANEL_PAD_X` / `UI_TITLEBAR_H` in `ui_render_windows()` with `ui_get_metrics()->` values for true resolution-adaptive rendering.
2. **Per-window dirty tracking:** Upgrade from single bounding-box to per-window dirty regions for more efficient partial presents during desktop interactions.
3. **Font profile "large" tier:** Add 3x3 or 4x4 scale for resolutions ≥2560x1440 and expose a `vfont` shell command to manually switch profiles.
4. **Overlay text cursor blink:** Use the pacing scheduler to drive a blinking cursor in the overlay plane, improving visual feedback for text input.
5. **Runtime pacing FPS control:** Add a `vfps <N>` shell command to dynamically adjust the pacing target (useful for benchmarking and DOS game workloads).
