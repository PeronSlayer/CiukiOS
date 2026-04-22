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
- `DONE` layout/alignment/readability hardening with resolution-aware layout metrics, clipping guards, font profiles and stricter GUI regression checks
- `DONE` render path performance and flicker reduction with overlay plane, dirty-present path and frame-pacing telemetry
- `PLANNED` windowing and app-launch UX that supports DOS app workflows

### Phase 5 - Milestone Target: Run DOOM
- `PLANNED` DOS extender runtime closure beyond M6 smoke skeleton (`DPMI` host services, protected-mode memory handoff, non-trivial extender validation)
- `PLANNED` BIOS/game runtime compatibility layer needed by the selected DOOM binary (`INT 10h`, `INT 16h`, `INT 1Ah`, `INT 2Fh`, remaining `INT 21h` subset)
- `PLANNED` filesystem/runtime configuration for assets, startup scripts and reproducible boot-to-game flow
- `PLANNED` graphics compatibility for the DOOM path (`VGA mode 13h` first, `VBE` subset only if required by chosen binary)
- `PLANNED` performance + input/audio expectations for playable session

### M6 - Protected Mode and DOS Extender Path
- `DONE` PMODE contract v1 baseline marker path + shell surface (`pmode`) with deterministic startup selftests
- `DONE` dedicated PMODE contract gate (`make test-m6-pmode`)
- `DONE` aggregate M6 readiness gate (`scripts/test_doom_readiness_m6.sh`) with transition-v2 check
- `DONE` reproducible M6 smoke executable (`CIUKPM.EXE` -> `0x36`) included in the OS image and validated by `make test-m6-smoke`
- `DONE` first DOS/4GW-like smoke executable (`CIUK4GW.EXE` -> `0x47`) using minimal DPMI host query `INT 2Fh AX=1687h`
- `DONE` real-mode entry point baseline (A20 probe/enable contract + descriptor baseline marker)
- `DONE` protected-mode transition contract v2 baseline (transition state block + descriptor snapshots + CR0 + return-path markers)
- `DONE` DOS/4GW host-interface skeleton baseline (DPMI detect, real-mode callback, interrupt reflection markers)
- `DONE` pmode memory accounting baseline domain with deterministic overlap guard
- `DONE` descriptor-return DPMI smoke (`CIUKDPM.EXE` -> `0x49`) validating non-zero `AX=1687h` host metadata
- `DONE` callable DPMI version smoke (`CIUK31.EXE` -> `0x4B`) validating `INT 31h AX=0400h`
- `DONE` bootstrap-facing DPMI smoke (`CIUK306.EXE` -> `0x4E`) validating `INT 31h AX=0306h`
- `DONE` allocate-LDT-descriptors DPMI smoke (`CIUKLDT.EXE` -> `0x52`) validating `INT 31h AX=0000h`
- `DONE` allocate-memory-block DPMI smoke (`CIUKMEM.EXE` -> `0x54`) validating `INT 31h AX=0501h`
- `DONE` VGA mode 13h compatibility scaffold with deterministic startup marker, `vga13` shell command and dedicated gate (`make test-vga13-baseline`)
- `DONE` BIOS compatibility surface markers (`INT 10h`, `INT 16h`, `INT 1Ah`, `INT 2Fh`) emitted at boot
- `DONE` deterministic DOOM asset packaging/discovery harness (`make test-doom-target-packaging`)
- `DONE` staged boot-to-DOOM failure-taxonomy harness (`make test-doom-boot-harness`)
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
- `DONE` extended non-interactive gate for `run` outcome classes (`ok/not_found/bad_format/runtime/unsupported_int21/args_parse`) via deterministic serial markers
- `DONE` launch-path parity mini-gate for `AH=4Ch` -> one-shot `AH=4Dh` behavior (`[ test ] dosrun status path selftest: PASS`)
- `DONE` minimal `.EXE MZ` single-program smoke (`CIUKMZ.EXE` -> `0x2B`) with reproducible host-side generator (`tools/mkciukmz_exe`) and dedicated gate (`make test-dosrun-mz`)
- `DONE` argv tail bridge with deterministic serial markers (`[dosrun] argv tail len=...`, `[dosrun] argv parse=PASS|FAIL`)
- `DONE` INT21h coverage extended to date/time (`AH=2Ah`, `AH=2Ch`) and IOCTL get-device-info (`AH=44h`/`AL=00h`) with boot-time `[compat]` markers

### SR-USER-TOOLING-001 - DOS User Tooling
Reference: `docs/sr-edit-001.md`

- `DONE` native CiukiOS line editor `CIUKEDIT.COM` for create/open/save `.TXT` files via INT 21h (`:w`, `:q`, `:wq`, `:l`, `:d N`, `:h`)

### SR-FS-002 - FAT32 Capability Track
Reference: `stage2/src/fat.c`

- `DONE` FAT mount diagnostics now expose filesystem type and FAT32 metadata marker (`fsinfo`, `next_free_hint`)
- `DONE` allocator now uses `next_free_hint` strategy (mount-time + runtime updates) instead of fixed linear scan from cluster 2
- `DONE` dynamic directory growth for non-fixed directories (including FAT32 root directory chain extension)
- `DONE` FSInfo `free cluster count` synchronization on allocation/free path (when FSInfo is valid and count is known)
- `DONE` new gate `test-fat32-progress` to validate FAT mount/FAT32 metadata markers
- `DONE` parity hardening for FAT32 edge semantics (FSInfo corruption fallback, hint sanitization, alloc/free sync and fixed-root guards) with `make test-fat32-edge`

### SR-M6-001 - Protected Mode / DOS Extender Readiness
References: `docs/m6-dos-extender-requirements.md`, M6 section above

- `DONE` PMODE contract startup marker + deterministic selftests
- `DONE` PMODE contract dedicated gate (`make test-m6-pmode`)
- `DONE` aggregate readiness orchestration gate (`scripts/test_doom_readiness_m6.sh`) including transition-v2 gate
- `DONE` transition path baseline contract (state block + snapshots + CR0/return-path markers)
- `DONE` DOS/4GW host-interface skeleton baseline (non-crashing deterministic markers)
- `DONE` pmode memory accounting baseline (isolated range + overlap guard)
- `DONE` reproducible M6 smoke executable (`CIUKPM.EXE` -> `0x36`) included in the OS image and covered by `make test-m6-smoke`
- `DONE` first callable DOS/4GW-like host-query smoke (`CIUK4GW.EXE` -> `0x47`) over minimal `INT 2Fh AX=1687h`
- `DONE` descriptor-return DPMI smoke (`CIUKDPM.EXE` -> `0x49`)
- `DONE` callable DPMI version smoke (`CIUK31.EXE` -> `0x4B`)
- `DONE` bootstrap-facing DPMI smoke (`CIUK306.EXE` -> `0x4E`)
- `DONE` allocate-LDT-descriptors DPMI smoke (`CIUKLDT.EXE` -> `0x52`)
- `DONE` allocate-memory-block DPMI smoke (`CIUKMEM.EXE` -> `0x54`)

### SR-OPENGEM-001 - OpenGEM Runtime Path
Reference: `docs/handoffs/2026-04-16-copilot-opengem-integration.md`

- `DONE` import pipeline (`scripts/import_opengem.sh`)
- `DONE` image composition gate (`CIUKIOS_INCLUDE_OPENGEM`)
- `DONE` shell command `opengem` preflight path
- `DONE` smoke test and image probe scripts
- `DONE` stricter behavioral tests for preflight/launch wiring and image composition via `make test-opengem`

### SR-GUI-001 - Desktop Usability and Stability
References: `docs/handoffs/2026-04-16-copilot-gui-v8-heavy-cycle.md`, related GUI handoffs

- `DONE` desktop scene + launcher baseline
- `DONE` regression marker harness (non-interactive)
- `DONE` alignment consistency across resolutions with layout metrics, clipping guards and stricter GUI regression checks
- `DONE` discoverability and interaction clarity with desktop help/hints, launcher dispatch markers and desktop-session readiness messaging
- `BACKLOG` richer desktop app model

### SR-DOOM-001 - First DOOM Milestone Path
Reference: `docs/roadmap-ciukios-doom.md`

- `DONE` freeze the first target binary/runtime pair as user-supplied shareware `DOOM.EXE` v1.9 + `DOOM1.WAD` (`DOOM.WAD` alias allowed only if mapped to the same shareware dataset), `DOS/4GW` class runtime, `mode 13h` video path, first success checkpoint = main menu reachable
- `IN PROGRESS` extend M6 from the current shallow DPMI baseline to a non-trivial DOS extender initialization path (`AX=1687h` descriptor-return slice + `CIUKDPM.EXE`, `INT 31h AX=0400h` callable smoke, `INT 31h AX=0306h` bootstrap smoke, and `INT 31h AX=0000h` allocate-LDT smoke are done; real extender execution is still pending)
- `PLANNED` validate protected-mode memory contracts for the chosen extender (`>1MB` allocation model, ownership, overlap safety, return path)
- `PLANNED` add a non-trivial DOS extender regression target beyond smoke markers and require it to reach interactive or near-interactive state
- `PLANNED` broaden BIOS/runtime compatibility tests against real traces used by installer/game startup (`INT 10h`, `16h`, `1Ah`, `2Fh`, remaining `INT 21h` subset)
- `PLANNED` add `VGA mode 13h` compatibility and framebuffer semantics for the first DOOM video path
- `IN PROGRESS` VGA mode 13h compatibility scaffold wired (shell `vga13`, startup marker, dedicated gate); real draw/render path still pending
- `PLANNED` add `VBE` subset only if the selected binary requires it
- `DONE` package executable, WAD/config files and launch scripts into the image with deterministic discovery rules (staged harness `make test-doom-boot-harness` active with `menu_reached` deferred until real runtime)
- `IN PROGRESS` add a dedicated boot-to-DOOM harness validating executable discovery, asset discovery, first frame or menu reachability, and failure taxonomy
- `PLANNED` tune keyboard path for gameplay expectations (latency, repeat, scan-code behavior, pause/resume stability)
- `PLANNED` add first audio baseline suitable for DOOM runtime expectations (likely Sound Blaster/QEMU-compatible path)
- `PLANNED` add milestone validation gates: main menu reachable, level load reachable, 10-minute gameplay smoke, documented known gaps

## Remaining Path To Milestone
1. Move M6 from the current descriptor-return plus version-query plus raw-mode bootstrap callable baseline to a real extender execution path and validate it against the frozen first target: user-supplied shareware `DOOM.EXE` v1.9 + `DOOM1.WAD`, `DOS/4GW` class runtime, `mode 13h`, minimum success = main menu reachable.
2. Broaden protected-mode memory handling so the chosen extender can allocate and use high memory safely without overlapping stage2/runtime state.
3. Expand BIOS/runtime compatibility coverage used by real startup paths, especially `INT 10h`, `INT 16h`, `INT 1Ah`, `INT 2Fh`, and the remaining `INT 21h` functions exposed during installer/game boot.
4. Implement the minimum `VGA mode 13h` graphics compatibility required to reach a real rendered frame for the frozen target.
5. Extend the existing deterministic FAT-image packaging/discovery baseline into a staged runtime harness.
6. Build a dedicated DOOM harness that classifies failure stages (`not found`, extender init fail, video init fail, asset fail, menu reached, level entered).
7. Tune keyboard semantics for gameplay and add the first audio-compatible baseline so the milestone is not just "boots" but "playable".
9. Add milestone gates for `main menu reachable`, `level load reachable`, and `10-minute gameplay smoke` before calling the milestone closed.

## Current Execution Focus
1. Advance milestone path toward first DOS DOOM boot and run.
2. Expand protected-mode and DOS-extender execution path beyond M6 baseline skeleton (real DOS/4GW compatibility path).
3. Build the first real DOOM-path harness around the selected executable/runtime pair instead of isolated smoke binaries.
4. Add the graphics, input and audio compatibility still missing for a playable milestone.
