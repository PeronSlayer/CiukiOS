# Phase 4 DOOM Gameplay Playable Milestone (2026-05-04)

## Objective
Record the Phase 4 runtime milestone where DOOM progresses from DOS extender bring-up to playable gameplay on the full FAT16 CiukiOS profile.

## Milestone Status
**CLOSED - GAMEPLAY PLAYABLE (2026-05-04)**

DOOM is no longer only a loader or video-initialization probe. The full-profile runtime reaches DOS/4GW, loads the WAD, initializes refresh/playloop/input/sound/HUD/status-bar paths, enters rendered gameplay, and was manually confirmed playable.

## Evidence
1. `make build-full`: PASS.
2. `make qemu-test-full`: PASS.
3. `DO_BUILD=0 DOOM_TAXONOMY_DISPLAY_MODE=none DOOM_TAXONOMY_SCREENSHOT=build/full/doom_post_status_after_rebuild.ppm QEMU_TIMEOUT_SEC=120 DOOM_TAXONOMY_OBSERVE_SEC=45 DOOM_TAXONOMY_MIN_STAGE=video_init make qemu-test-full-doom-taxonomy`: PASS.
4. Visual artifact: `build/full/doom_post_status_after_rebuild.png`, 640x400, shows DOOM gameplay viewport and HUD after status-bar initialization.
5. Manual validation: project owner confirmed DOOM is playable interactively on the generated full-profile image.

## Technical Highlights
1. The full-profile INT 21h memory arena now uses an ordered MCB table as the allocator source of truth for allocation, free, and resize operations.
2. DOS/4GW compatibility was unblocked by preserving FAT16 read/seek return values across handle-slot cleanup.
3. The ordered MCB table was expanded to support later DOOM runtime allocation pressure.
4. The DOOM taxonomy harness launches from `\APPS\DOOM`, preserving relative WAD discovery.
5. The taxonomy can run a visual headless lane with QEMU `-display none` plus monitor `screendump` capture, avoiding the host `-nographic` graphics-transition crash path.
6. `menu_reached` remains a conservative serial marker and no longer treats startup `M_Init` as menu proof.

## Scope Notes
1. Proprietary DOOM assets remain local-only under `third_party/Doom` and are not project-public deliverables.
2. The milestone proves native runtime compatibility progress, not CPU emulation.
3. Audio hardware compatibility is still partial; logs show Sound Blaster probing is not fully satisfied, but gameplay remains playable.
4. Future work should add a separate `gameplay_visible` or `gameplay_playable` taxonomy stage instead of overloading `menu_reached`.

## Release Mapping
This milestone is recorded as `CiukiOS pre-Alpha v0.6.1`.
