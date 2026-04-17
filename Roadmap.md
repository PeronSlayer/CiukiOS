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
- `DONE` dynamic backbuffer policy up to `1920x1080` with dedicated policy gate
- `DONE` scaling strategy beyond Full HD and mode-policy hardening across wider GOP catalogs

### Phase 2 - DOS Core Compatibility
- `DONE` INT21h baseline set (priority-A matrix path)
- `DONE` FAT-backed file handle baseline
- `DONE` FAT32 capability step-up (FSInfo-backed allocation hint + dynamic directory growth for non-fixed directories)
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

### M6 - Protected Mode and DOS Extender Path
- `DONE` PMODE contract v1 baseline marker path + shell surface (`pmode`) with deterministic startup selftests
- `DONE` dedicated PMODE contract gate (`make test-m6-pmode`)
- `DONE` aggregate M6 readiness gate (`scripts/test_doom_readiness_m6.sh`) with transition-v2 check
- `DONE` real-mode entry point baseline (A20 probe/enable contract + descriptor baseline marker)
- `DONE` protected-mode transition contract v2 baseline (transition state block + descriptor snapshots + CR0 + return-path markers)
- `DONE` DOS/4GW host-interface skeleton baseline (DPMI detect, real-mode callback, interrupt reflection markers)
- `DONE` pmode memory accounting baseline domain with deterministic overlap guard
- Gate: `scripts/test_doom_readiness_m6.sh` PASS + no regressions to INT21h/MZ/shell/video
- Ref: `docs/m6-dos-extender-requirements.md`

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
- `DONE` persistent boot config runtime store in CMOS with checksum validation (`magic/version/flags/mode/width/height/crc32`)
- `DONE` loader precedence for mode selection: `CMOS` -> `VMODE.CFG` -> policy/default with deterministic source marker
- `DONE` reboot persistence gate for mode selection (`make test-vmode-persistence`)
- `DONE` dedicated non-interactive regression gate (`make test-video-mode`)
- `DONE` minimum compatibility target raised to at least `1024x768` (shared driver limits + loader mode policy)
- `DONE` deterministic compatibility gate for `1024x768` (`make test-video-1024`)
- `DONE` runtime loader marker for `1024x768` policy result (`GOP: policy1024 ... result=PASS/FAIL`) validated by video pipeline gate
- `DONE` dynamic/larger backbuffer policy up to `1920x1080` (`scripts/test_video_backbuf_policy.sh`)
- `DONE` compatibility expansion above Full HD without direct-render fallback
- `DONE` deterministic frame pacing / present scheduler baseline to reduce jitter under GUI workloads
- `DONE` overlay text plane baseline so shell/UI text remains readable during gfx redraw cycles
- `DONE` resolution-independent layout metrics baseline for desktop widgets/panels (800x600 -> 1920x1080)
- `DONE` font profile support by resolution class (`small`/`normal`)
- `DONE` dedicated regression gate for advanced video+UI path (`make test-video-ui-v2`)

### SR-DOSRUN-001 - First Simple DOS Program Execution
Reference: `docs/subroadmap-sr-dosrun-001.md`

- `DONE` COM runtime baseline and shell `run` command are active
- `DONE` deterministic end-to-end smoke path for launching a simple DOS program (`CIUKSMK.COM`) and validating return status (`0x2A`)
- `DONE` compact non-interactive gate for `run` outcome classes (`ok/not_found/bad_format/runtime`) via deterministic serial markers
- `DONE` launch-path parity mini-gate for `AH=4Ch` -> one-shot `AH=4Dh` behavior (`[ test ] dosrun status path selftest: PASS`)
- `IN PROGRESS` minimal `.EXE MZ` single-program smoke integrated with existing MZ regression path

### SR-FS-002 - FAT32 Capability Track
Reference: `stage2/src/fat.c`

- `DONE` FAT mount diagnostics now expose filesystem type and FAT32 metadata marker (`fsinfo`, `next_free_hint`)
- `DONE` allocator now uses `next_free_hint` strategy (mount-time + runtime updates) instead of fixed linear scan from cluster 2
- `DONE` dynamic directory growth for non-fixed directories (including FAT32 root directory chain extension)
- `DONE` FSInfo `free cluster count` synchronization on allocation/free path (when FSInfo is valid and count is known)
- `DONE` new gate `test-fat32-progress` to validate FAT mount/FAT32 metadata markers
- `IN PROGRESS` parity hardening for FAT32 edge semantics (broader stress scenarios and corruption fallback behavior)

### SR-M6-001 - Protected Mode / DOS Extender Readiness
References: `docs/m6-dos-extender-requirements.md`, M6 section above

- `DONE` PMODE contract startup marker + deterministic selftests
- `DONE` PMODE contract dedicated gate (`make test-m6-pmode`)
- `DONE` aggregate readiness orchestration gate (`scripts/test_doom_readiness_m6.sh`) including transition-v2 gate
- `DONE` transition path baseline contract (state block + snapshots + CR0/return-path markers)
- `DONE` DOS/4GW host-interface skeleton baseline (non-crashing deterministic markers)
- `DONE` pmode memory accounting baseline (isolated range + overlap guard)
- `IN PROGRESS` real DOS/4GW compatibility beyond skeleton baseline

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
2. Expand protected-mode and DOS-extender execution path beyond M6 baseline skeleton (real DOS/4GW compatibility path).
3. Evolve video subsystem beyond Full HD baseline while preserving current deterministic gates.
4. Stabilize FAT32 behavior as default filesystem baseline for upcoming DOS runtime steps.
