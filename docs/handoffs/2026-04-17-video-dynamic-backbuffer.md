# Handoff: Video Dynamic Backbuffer and Extended Resolution Support

**Date:** 2026-04-17
**Author:** Copilot (video track)
**Baseline:** CiukiOS Alpha v0.6.0

## Context and Goal

The video subsystem was hard-capped at 1024x768 for double-buffering. This change raises the backbuffer budget to 1920x1080 (Full HD), allows VMODE.CFG to select any 32bpp mode regardless of backbuffer fit, and adds comprehensive diagnostics and test coverage for the new behavior.

## Files Touched

| File | Change |
|------|--------|
| `boot/proto/video_limits.h` | MAX_W/H raised to 1920x1080; added `VIDEO_POLICY_BASELINE_W/H` constants |
| `boot/uefi-loader/loader.c` | VMODE.CFG no longer gated on `fits_backbuf`; policy baseline uses constants; backbuf budget diagnostic added |
| `stage2/src/video.c` | Backbuffer budget/needed/fits serial diagnostic added at init |
| `scripts/test_video_1024_compat.sh` | Extended: baseline policy, backbuf budget, VMODE.CFG gate checks |
| `scripts/test_video_mode_pipeline.sh` | New required patterns: `GOP: backbuf budget=`, `[video] backbuf_budget=` |
| `scripts/test_video_backbuf_policy.sh` | **New.** Static analysis gate for backbuffer policy consistency |
| `Makefile` | Added `test-video-backbuf` target |
| `docs/roadmap-ciukios-doom.md` | Current Snapshot updated |

## Decisions Made

1. **BSS budget 1920x1080x4 = ~8 MB.** Stage2 loads at 128 MB with 512 MB RAM — ample headroom. No linker/loader ABI change needed.
2. **VMODE.CFG can select any 32bpp mode.** This allows explicit user override to resolutions even beyond the backbuffer (driver falls back to direct rendering). The fallback preference table still requires `fits_backbuf`.
3. **Baseline policy remains 1024x768.** The `VIDEO_POLICY_BASELINE_W/H` constants centralize this invariant. The `policy1024` marker is unaffected.
4. **Three test layers:**
   - `test-video-1024` (static): verifies constants, budget, preference table, VMODE.CFG gate
   - `test-video-backbuf` (static): verifies backbuffer allocation, dynamic decision, diagnostics
   - `test-video-mode` (QEMU runtime): verifies both loader and stage2 emit budget diagnostics

## Validation Performed

- `make all` — build passes
- `make test-video-1024` — static gate passes
- `make test-video-backbuf` — new static gate passes
- `make test-mz-regression` — no regression
- `make test-phase2` — no regression

## Risks and Next Steps

1. **QEMU default GOP** may not offer modes above 1024x768 without explicit `-device VGA,vgamem_mb=X`. The current QEMU config works for 1024x768 default. To validate higher resolutions at runtime, the QEMU invocation may need adjustment.
2. **Direct rendering path** (when resolution exceeds budget) has no dirty-rect optimization. For DOOM's VGA 320x200 target this is irrelevant, but desktop UI at >1080p would benefit from future work.
3. Next video milestone: VGA mode 13h compatibility for M7 (DOOM graphics path).
