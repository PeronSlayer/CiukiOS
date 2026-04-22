# Handoff: Video Driver Review + Roadmap/Version Sync

**Date:** 2026-04-16  
**Author:** Codex  
**Base branch:** `feature/copilot-video-driver-minimal`

## Scope
Post-review hardening and documentation sync after Copilot's "minimal video driver" delivery.

## What was validated
1. `make test-stage2` -> PASS
2. `make test-freedos-pipeline` -> PASS
3. `make test-opengem` -> PASS (currently smoke-only semantics)

## Review fixes applied
1. **UEFI GOP mode selection ordering**
   - Changed preferred list to start from `800x600` so current static backbuffer can remain active where possible.
   - File: `boot/uefi-loader/loader.c`

2. **UEFI QueryMode cleanup**
   - Added `BS->FreePool(mode_info)` in GOP mode enumeration loop.
   - File: `boot/uefi-loader/loader.c`

3. **Splash clipping centering**
   - When scanline renderer clamps width to 800, recompute `off_x` to keep the image centered.
   - File: `stage2/src/splash.c`

## Docs/version updates
1. Added unified roadmap file:
   - `Roadmap.md`
   - Contains main roadmap + sub-roadmaps (`SR-VIDEO-001`, `SR-OPENGEM-001`, `SR-GUI-001`) with status tags.

2. Updated README:
   - Bumped visible version to `v0.5.5`
   - Added changelog entry for video driver pass + roadmap integration
   - Added `Roadmap.md` to key docs

3. Synced stage2 runtime version string:
   - `stage2/include/version.h` -> `Alpha v0.5.5`

## Notes / residual risk
1. Backbuffer is still static 800x600; higher selected modes fall back to direct rendering.
2. `test-opengem` currently reports warnings for missing runtime launch markers but still passes (smoke-only gate).

