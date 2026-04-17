![Splashscreen CiukiOS](misc/CiukiOS_SplashScreen.png)

# CiukiOS

Open Source RetroOS project built from scratch.
Mission: become a progressively more complete environment capable of running DOS, FreeDOS and pre-NT Windows software over time.

## Current Version
`CiukiOS Alpha v0.6.3`
Focus: compatibility foundation + progressive desktop/runtime improvements.

## Changelog (Latest)
### v0.6.3
1. Expanded video subsystem with overlay plane support, frame pacing counters, and deterministic present-mode telemetry.
2. Added layout metrics v3 and adaptive font profiles (`small`/`normal`) to improve UI readability across multiple resolutions.
3. Added video/UI regression gate `make test-video-ui-v2` (`scripts/test_video_ui_regression_v2.sh`).
4. Added deterministic DOS smoke payload `CIUKSMK.COM` and integrated `run` outcome markers (`ok/not_found/bad_format/runtime`).
5. Added end-to-end DOS run gate `make test-dosrun-simple` (`scripts/test_dosrun_simple_program.sh`) validating launch + return code path.
6. Fixed DOS run selftest contract to validate runtime-native `AH=4Ch -> AH=4Dh` one-shot status semantics.
7. Added CMOS-backed persistent boot video configuration with integrity checks and loader source precedence (`CMOS` -> `VMODE.CFG` -> policy), including reboot persistence gate `make test-vmode-persistence`.
8. Closed M6 baseline with deterministic transition-state/A20/descriptor/host-skeleton/pmem markers and expanded M6 closure gates.

### v0.6.2
1. Improved FAT layer toward FAT32 baseline with mount metadata marker (`type/fsinfo/next_free_hint`) in stage2 boot logs.
2. Added hint-based free-cluster allocation strategy (`next_free_hint`) instead of fixed scan from cluster 2.
3. Added dynamic directory-chain expansion for non-fixed directories (important for FAT32 root/subdirectory growth).
4. Added regression gate `make test-fat32-progress` (`scripts/test_fat32_progress.sh`).
5. Expanded main roadmap with new sub-roadmaps: `SR-DOSRUN-001` (simple DOS program milestone) and `SR-FS-002` (FAT32 capability track).

### v0.6.1
1. Added M6 protected-mode contract baseline selftests at startup with explicit PASS/FAIL markers.
2. Added dedicated gate `make test-m6-pmode` (`scripts/test_m6_pmode_contract.sh`).
3. Added M6 requirements document: `docs/m6-dos-extender-requirements.md`.
4. Added aggregate M6 readiness gate: `scripts/test_doom_readiness_m6.sh` (phase2 + freedos + video + m6 gates).
5. Refreshed `third_party/freedos/runtime-manifest.csv` to restore reproducibility checks in pipeline validation.
6. Updated roadmap and sub-roadmaps to reflect M6 activation and current video/backbuffer status.

### v0.6.0
1. Merged INT21 compatibility expansion with `AH=56h` rename (same-directory DOS-like subset).
2. Extended INT21 FAT end-to-end selftest coverage to include rename path validation.
3. Synced INT21 compatibility matrix and matrix gate with function `56h`.
4. Integrated video mode stack: GOP mode catalog handoff, `VMODE.CFG` persistence, and shell command surface `vmode`/`vres`.
5. Added dedicated regression gate `make test-video-mode` and hardened execution with QEMU lock serialization.
6. Updated stage2 runtime version string to match current alpha.
7. Improved EXE/MZ runtime compatibility and strengthened deterministic regression coverage.

### v0.5.5
1. Integrated the first minimal video driver pass with double buffering and explicit `video_present()` flow.
2. Added scanline blitting path for splash rendering to reduce per-pixel overhead.
3. Added GOP mode selection hardening in loader (preferred mode order + `QueryMode` cleanup).
4. Added central roadmap file with main milestones and sub-roadmap tracking.
5. Synced stage2 runtime version string with README version.

### v0.5.4
1. Added optional OpenGEM (FreeGEM) integration flow (`import`, runtime composition, pipeline gate, smoke test, image probe).
2. Added shell `opengem` command with preflight checks and multi-entry launch path detection.
3. Added OpenGEM provenance, ops and licensing notes in project docs.
4. Kept FreeDOS pipeline compatibility and automated validation green.

### v0.5.2
1. Updated project purpose and public positioning as Open Source RetroOS.
2. Added collaboration and contribution direction in README.
3. Added explicit development pace note (spare-time project).
4. Added donation/support section and dedicated donation file.
5. Moved internal LLM collaboration/handoff docs to local-only workflow.
6. Introduced pre-1.0 alpha policy for releases and build instructions.

### v0.5.1
1. Improved desktop readability with layout grid v2 and clearer window chrome.
2. Upgraded desktop interaction flow (focus/navigation feedback and launcher clarity).
3. Added launcher/dock visual pass v2 with better selection visibility.
4. Added GUI regression helper script: `make test-gui-desktop`.
5. Added Copilot handoffs for desktop polish tasks D1-D5.

### v0.5
1. Added INT21 compatibility set for console/drive/DTA paths (`AH=06h/07h/0Ah/0Eh/1Ah/2Fh`) with deterministic tests.
2. Extended boot/test gates for INT21 matrix and compatibility markers.
3. Added interactive desktop session from shell (`desktop` command).
4. Added desktop controls (`TAB`, `UP/DOWN`, `J/K`, `ENTER`, `ESC`) and startup hint for GUI testing.
5. Kept boot/fallback/FAT/INT21 automated regression flow green.

### v0.4
1. Added graphic splash renderer (framebuffer, centered scaling, ASCII-to-grayscale mapping).
2. Added explicit framebuffer metadata in stage handoff ABI.
3. Added shell preview command: `gsplash` (alias `splash`).
4. Kept ASCII splash as automatic fallback path.

## Current Direction
The active north star is:
1. Run real DOS executables on CiukiOS.
2. Reach the first major game milestone: run DOS DOOM from CiukiOS.
3. Extend compatibility toward DOS, FreeDOS and pre-NT Windows software in incremental phases.

## Open Source Collaboration
CiukiOS is open to collaborative proposals, issue reports, technical discussion and PR contributions.
If you want to help, please open an issue with:
1. clear problem/idea description
2. expected behavior
3. reproducible steps or technical context

## Development Pace
This is a spare-time project.
Updates are continuous but not on a fixed schedule: progress depends on available free time and mood.

## Alpha Policy (Pre-1.0)
Until `CiukiOS Alpha v1.0`, this project follows these rules:
1. No official prebuilt release artifacts are provided.
2. No public step-by-step build instructions are provided in this README.
3. Development is currently heavily assisted by LLM tooling (OpenAI, Claude, Copilot) while core engineering skills and architecture mature.
4. Versioning cadence: every 2/3 integrated updates bump patch version automatically (`x.y.z -> x.y.(z+1)`); milestone-sized integrations may bump minor version.

## Key Docs
1. Unified roadmap and sub-roadmaps: `Roadmap.md`
2. DOS-to-DOOM roadmap: `docs/roadmap-ciukios-doom.md`
3. DOS 6.2 compatibility roadmap: `docs/roadmap-dos62-compat.md`
4. FreeDOS integration and licensing policy: `docs/freedos-integration-policy.md`
5. FreeDOS symbiotic architecture: `docs/freedos-symbiotic-architecture.md`
6. OpenGEM integration notes and operations: `docs/opengem-integration-notes.md`, `docs/opengem-ops.md`
7. Shared contributor/session notes: `CLAUDE.md`

## Third-Party and Licensing (FreeDOS + OpenGEM Notice)
1. This repository can include and use third-party FreeDOS components in `third_party/freedos/`.
2. FreeDOS packages are distributed under their own licenses (often GPL-family, but not a single license for all files).
3. OpenGEM is integrated as an optional GUI payload in the FreeDOS runtime path (`third_party/freedos/runtime/OPENGEM/`), licensed under GPL-2.0-or-later.
4. Keep license/provenance files with imported components and validate redistribution rights per package.
5. See:
   - `docs/freedos-integration-policy.md`
   - `docs/opengem-integration-notes.md`
   - `docs/opengem-ops.md`
   - `docs/legal/freedos-licenses/`

## Donations and Support
If you want to support CiukiOS development (including recurring LLM/tooling costs such as OpenAI, Claude and Copilot subscriptions), see:
- `DONATIONS.md`

Provider selection is currently in progress to choose the most convenient and transparent option for contributors.

## Credits
Developed collaboratively with Claude Code,Codex(Openai) and Github Copilot.

The name **CiukiOS** comes from a private joke between me and my girlfriend about our dog Jack (Jacky), who is no longer with us.
His nickname was **Ciuk/Ciuki**, and we used to joke that if we ever built an operating system, we would call it **CiukiOS**.

So this is why is dedicated to one of the best dogs i ever met, Jack.
