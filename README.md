![Splashscreen CiukiOS](misc/CiukiOS_SplashScreen.png)

# CiukiOS

Open Source RetroOS project built from scratch.
Mission: become a progressively more complete environment capable of running DOS, FreeDOS and pre-NT Windows software over time.

## Current Version
`CiukiOS Alpha v0.6.9`
Focus: compatibility foundation + progressive desktop/runtime improvements.

## Index
1. Full documentation: `documentation.md`
2. Full changelog: `CHANGELOG.md`
3. High-level roadmap: `Roadmap.md`
4. Detailed DOOM roadmap: `docs/roadmap-ciukios-doom.md`
5. Donations and support: `DONATIONS.md`

## Changelog (Latest)
### v0.6.9
1. Added deterministic startup-chain gate `make test-startup-chain`, covering `CONFIG.SYS`, `AUTOEXEC.BAT`, `.BAT` labels/`goto`/`if errorlevel`, env expansion and FreeDOS startup-file image wiring.
2. Added FAT32 edge-semantics gate `make test-fat32-edge`, covering FSInfo corruption fallback, hint sanitization, alloc/free sync and fixed-root overflow guards.
3. Strengthened GUI and OpenGEM regression coverage so layout/discoverability contracts and OpenGEM preflight/launch wiring are validated explicitly instead of relying on documentation drift.
4. Closed the remaining roadmap items that were still marked `IN PROGRESS` for the current UI/FAT/startup baseline and synchronized roadmap docs to the validated implementation state.

### v0.6.8
1. Added the first DOS/4GW-like smoke executable `CIUK4GW.EXE`, included in the OS image and returning deterministic code `0x47` after a minimal DPMI host query.
2. Extended the services ABI and stage2 DOS runtime with a minimal callable `INT 2Fh AX=1687h` host-query slice for DOS extender smoke validation.
3. Added dedicated gate `make test-m6-dos4gw-smoke` and wired it into the aggregate M6 readiness orchestration.
4. M6 now validates both a generic protected-mode readiness smoke (`CIUKPM.EXE`) and a first DOS/4GW-like host-query smoke (`CIUK4GW.EXE`).

Full changelog: `CHANGELOG.md`

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
1. Central project documentation: `documentation.md`
2. Full changelog: `CHANGELOG.md`
3. Unified roadmap and sub-roadmaps: `Roadmap.md`
4. DOS-to-DOOM roadmap: `docs/roadmap-ciukios-doom.md`
5. DOS 6.2 compatibility roadmap: `docs/roadmap-dos62-compat.md`
6. FreeDOS integration and licensing policy: `docs/freedos-integration-policy.md`
7. FreeDOS symbiotic architecture: `docs/freedos-symbiotic-architecture.md`
8. OpenGEM integration notes and operations: `docs/opengem-integration-notes.md`, `docs/opengem-ops.md`
9. Shared contributor/session notes: `CLAUDE.md`

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
