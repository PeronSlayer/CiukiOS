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
- `DONE` EXE/MZ runtime compatibility depth (deterministic parser/relocation hardening + regression suite)
- `DONE` compatibility harness expansion against real DOS app corpus (`make test-mz-corpus`, included in `make test-phase2`)

### Phase 3 - Symbiotic FreeDOS Integration
- `DONE` FreeDOS runtime import + pipeline validation
- `DONE` OpenGEM optional integration flow
- `DONE` richer runtime bundle composition and packaging reliability (deterministic runtime manifest + pipeline reproducibility check)
- `DONE` upstream sync automation and reproducible import manifests (`freedos-sync-upstreams`, `third_party/freedos/upstreams.lock`)

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
- `DONE` minimum compatibility target raised to at least `1024x768` (shared driver limits + loader mode policy)
- `DONE` deterministic compatibility gate for `1024x768` (`make test-video-1024`)
- `DONE` runtime loader marker for `1024x768` policy result (`GOP: policy1024 ... result=PASS/FAIL`) validated by video pipeline gate
- `PLANNED` larger/dynamic backbuffer allocation policy
- `PLANNED` compatibility expansion above `1024x768` without direct-render fallback

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
1. Advance milestone path toward first DOS DOOM boot and run.
2. Expand protected-mode and DOS-extender execution path.
3. Evolve video subsystem beyond current 1024x768 baseline (dynamic/larger backbuffer policy).
