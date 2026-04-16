# CiukiOS Roadmap

This file is the single high-level tracker for CiukiOS.
It complements detailed docs in `docs/` and handoffs in `docs/handoffs/`.

## Status Legend
- `DONE`: implemented and validated in current mainline
- `IN PROGRESS`: partially implemented, still evolving
- `PLANNED`: approved direction, not implemented yet
- `BACKLOG`: future candidate

## Main Roadmap (North Star: DOS DOOM on CiukiOS)

### Phase 1 - Boot and Runtime Foundation
- `DONE` UEFI loader + stage2 handoff
- `DONE` stable fallback boot path and stage2 scaffolding tests
- `DONE` timer/keyboard/interrupt baseline
- `DONE` framebuffer text+gfx baseline
- `DONE` first video driver vmode stack (`double buffer`, blitting, mode catalog + `vmode` command + pipeline gate)
- `IN PROGRESS` dynamic mode scaling and backbuffer policy across higher resolutions

### Phase 2 - DOS Core Compatibility
- `DONE` INT21h baseline set (priority-A matrix path)
- `DONE` FAT-backed file handle baseline
- `DONE` INT21h file search + rename subset (`AH=4Eh/4Fh/56h`) with matrix/test coverage
- `DONE` COM runtime and shell command surface
- `IN PROGRESS` EXE/MZ runtime compatibility depth
- `PLANNED` compatibility harness expansion against real DOS apps

### Phase 3 - Symbiotic FreeDOS Integration
- `DONE` FreeDOS runtime import + pipeline validation
- `DONE` OpenGEM optional integration flow
- `IN PROGRESS` richer runtime bundle composition and packaging reliability
- `PLANNED` upstream sync automation and reproducible import manifests

### Phase 4 - UX and Desktop Layer
- `DONE` desktop scene baseline and interaction shell
- `IN PROGRESS` layout/alignment/readability hardening
- `IN PROGRESS` render path performance and flicker reduction
- `PLANNED` windowing and app-launch UX that supports DOS app workflows

### Phase 5 - Milestone Target: Run DOOM
- `PLANNED` executable/runtime compatibility needed by DOS DOOM
- `PLANNED` filesystem/runtime configuration for assets and launch scripts
- `PLANNED` performance + input/audio expectations for playable session

## Sub-Roadmaps

### SR-VIDEO-001 - First Video Driver Pass
Reference: `docs/handoffs/2026-04-16-video-driver-minimal.md`

- `DONE` render-target indirection (`back buffer` vs direct framebuffer)
- `DONE` explicit present path (`video_present()`)
- `DONE` scanline blit API (`video_blit_row()`)
- `DONE` splash rendering converted to scanline writes
- `DONE` GOP mode selection baseline in UEFI loader
- `DONE` dirty-rect tracking + incremental present
- `DONE` persisted mode configuration (`VMODE.CFG`) and shell utility (`vmode`/`vres`)
- `DONE` dedicated non-interactive regression gate (`make test-video-mode`)
- `PLANNED` larger/dynamic backbuffer allocation policy

### SR-OPENGEM-001 - OpenGEM Runtime Path
Reference: `docs/handoffs/2026-04-16-copilot-opengem-integration.md`

- `DONE` import pipeline (`scripts/import_opengem.sh`)
- `DONE` image composition gate (`CIUKIOS_INCLUDE_OPENGEM`)
- `DONE` shell command `opengem` preflight path
- `DONE` smoke test and image probe scripts
- `IN PROGRESS` stricter behavioral tests in interactive flow

### SR-GUI-001 - Desktop Usability and Stability
References: `docs/handoffs/2026-04-16-copilot-gui-v8-heavy-cycle.md`, related GUI handoffs

- `DONE` desktop scene + launcher baseline
- `DONE` regression marker harness (non-interactive)
- `IN PROGRESS` alignment consistency across resolutions
- `IN PROGRESS` discoverability and interaction clarity
- `BACKLOG` richer desktop app model

## Current Execution Focus
1. Expand EXE/MZ compatibility depth and process/runtime semantics for real DOS binaries.
2. Improve DOS/FreeDOS app runtime coverage (beyond synthetic selftests) with deterministic harnesses.
3. Advance milestone path toward first DOS DOOM boot and run.
