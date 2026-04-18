![Splashscreen CiukiOS](misc/CiukiOS_SplashScreen.png)

# CiukiOS

Open Source RetroOS project built from scratch.
Mission: become a progressively more complete environment capable of running DOS, FreeDOS and pre-NT Windows software over time.

## Quick Links
1. Project roadmap: [Roadmap.md](Roadmap.md)
2. Detailed DOOM roadmap: [docs/roadmap-ciukios-doom.md](docs/roadmap-ciukios-doom.md)
3. Donations and support: [DONATIONS.md](DONATIONS.md)
4. Full documentation: [documentation.md](documentation.md)
5. Full changelog: [CHANGELOG.md](CHANGELOG.md)

## Current Version
`CiukiOS Alpha v0.8.6`
Focus: compatibility foundation + progressive desktop/runtime improvements.

## Changelog (Latest)
### v0.8.6
1. Released the latest shell UX set: path-aware direct execution, richer `which` / `where` / `resolve`, stronger completion flow, and extended DOS-like editing shortcuts.
2. Turned VGA mode `0x13` from a scaffold into a first real runtime checkpoint with deterministic markers in the gfx path and a richer `DOSMODE13.COM` validation frame.
3. Upgraded `test-vga13-baseline` into a runtime-aware gate with static fallback only when host capture is incomplete.
4. Added the next M6 DPMI step: stateful memory allocation/free tracking plus new `CIUKREL.EXE` smoke coverage for `INT 31h AX=0502h`.
5. Wired the new M6 smoke through image packaging, aggregate readiness gating, and supporting roadmap/documentation updates.
6. Bumped baseline version to `CiukiOS Alpha v0.8.6`.

![DOSMODE13.COM — first real mode 0x13 runtime checkpoint validated on QEMU (v0.8.6)](misc/screenshots/v0.8.6-dosmode13-runtime-checkpoint.png)

### v0.8.5
1. Added signed/clipped mode 0x13 patch placement helpers: `gfx_mode13_blit_indexed_clip(...)` for opaque/masked indexed blits with automatic off-screen crop and `gfx_mode13_blit_scaled_clip(...)` for stable nearest-neighbor scaled patches with signed destination coordinates.
2. Added `gfx_mode13_draw_column_sampled_masked(...)`, a DOOM-leaning sampled masked column primitive that uses 16.16 source stepping and clips signed destination Y.
3. Extended `ciuki_gfx_services_t` with `mode13_blit_indexed_clip`, `mode13_blit_scaled_clip`, and `mode13_draw_column_sampled_masked`, preserving append-only ABI growth.
4. Updated `GFXDOOM.COM` to validate real patch placement cases: one clipped top-left scaled patch, one centered patch, and stretched sampled masked columns across the lower half of the screen.
5. Bumped baseline version to `CiukiOS Alpha v0.8.5`.

### v0.8.4
1. Added the next DOOM-facing mode 0x13 helpers: `gfx_mode13_blit_scaled(...)` for nearest-neighbor scaled indexed blits (HUD/title patch style), `gfx_mode13_draw_column_masked(...)` for transparent single-column draws, and `gfx_frame_counter()` for present-count pacing / instrumentation.
2. Extended `ciuki_gfx_services_t` with `mode13_blit_scaled`, `mode13_draw_column_masked`, and `frame_counter`, keeping the ABI append-only before `reserved[32]`.
3. New sample `GFXDOOM.COM` (`com/gfxdoom/`): fills a mode 0x13 background, scales a 16x16 indexed patch to 160x100, overlays masked columns, presents after each stage, and prints `[gfxdoom] frames=<n>` + `[gfxdoom] OK`.
4. `run_ciukios.sh` now copies `GFXDOOM.COM` into the FAT image so the demo is runnable directly from the shell.
5. Bumped baseline version to `CiukiOS Alpha v0.8.4`.

Full changelog: [CHANGELOG.md](CHANGELOG.md)

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
1. Central project documentation: [documentation.md](documentation.md)
2. Full changelog: [CHANGELOG.md](CHANGELOG.md)
3. Unified roadmap and sub-roadmaps: [Roadmap.md](Roadmap.md)
4. DOS-to-DOOM roadmap: [docs/roadmap-ciukios-doom.md](docs/roadmap-ciukios-doom.md)
5. DOS 6.2 compatibility roadmap: [docs/roadmap-dos62-compat.md](docs/roadmap-dos62-compat.md)
6. FreeDOS integration and licensing policy: [docs/freedos-integration-policy.md](docs/freedos-integration-policy.md)
7. FreeDOS symbiotic architecture: [docs/freedos-symbiotic-architecture.md](docs/freedos-symbiotic-architecture.md)
8. OpenGEM integration notes and operations: [docs/opengem-integration-notes.md](docs/opengem-integration-notes.md), [docs/opengem-ops.md](docs/opengem-ops.md)
9. Shared contributor/session notes: [CLAUDE.md](CLAUDE.md)

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
- [DONATIONS.md](DONATIONS.md)

Provider selection is currently in progress to choose the most convenient and transparent option for contributors.

## Credits
Developed collaboratively with Claude Code,Codex(Openai) and Github Copilot.

The name **CiukiOS** comes from a private joke between me and my girlfriend about our dog Jack (Jacky), who is no longer with us.
His nickname was **Ciuk/Ciuki**, and we used to joke that if we ever built an operating system, we would call it **CiukiOS**.

So this is why is dedicated to one of the best dogs i ever met, Jack.
