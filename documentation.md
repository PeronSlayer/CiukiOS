# CiukiOS Documentation

## Purpose
This file is the central human-readable documentation hub for CiukiOS.
It complements the roadmap files, the full changelog, and the per-task handoff files under `docs/handoffs/`.

Maintenance rule:
1. Update this file whenever a completed task changes the externally visible project state, architecture, validation workflow, or milestone status.
2. Keep writing handoffs for major changes, but also merge the durable outcome here so the current project state stays easy to read without replaying every handoff.
3. Treat this file as the stable project narrative and the handoffs as the detailed session log.

## Project Summary
CiukiOS is an open source RetroOS built from scratch with the current north star of running real DOS software, then reaching a first real DOS DOOM milestone.

The current direction is:
1. Execute real `.COM` and `.EXE MZ` DOS binaries.
2. Expand DOS compatibility through deterministic test gates.
3. Grow protected-mode and DOS extender support incrementally.
4. Reach a reproducible boot-to-DOOM path.
5. Extend compatibility over time toward FreeDOS and pre-NT Windows software.

## Primary Index
1. Full changelog: [CHANGELOG.md](CHANGELOG.md)
2. High-level roadmap: [Roadmap.md](Roadmap.md)
3. Detailed DOOM roadmap: [docs/roadmap-ciukios-doom.md](docs/roadmap-ciukios-doom.md)
4. DOS 6.2 compatibility roadmap: [docs/roadmap-dos62-compat.md](docs/roadmap-dos62-compat.md)
5. Donation/support info: [DONATIONS.md](DONATIONS.md)
6. Collaboration/session context: [CLAUDE.md](CLAUDE.md)
7. Detailed handoff log: [docs/handoffs/README.md](docs/handoffs/README.md)

## Current Version
Current public version in README: `CiukiOS Alpha v0.8.0`

## Current Project State

### Boot and runtime
1. UEFI loader boots stage2 reliably.
2. Fallback path to `kernel.elf` remains available and tested.
3. Splashscreen rendering is integrated.
4. Stage2 provides a shell and DOS-like runtime surface.

### DOS execution baseline
1. `.COM` execution is active with PSP-style semantics.
2. `.EXE MZ` execution is active with relocation and regression coverage.
3. DOS termination/status flow is wired through `INT 20h`, `INT 21h AH=4Ch`, and one-shot status retrieval semantics.
4. Startup-chain support covers `CONFIG.SYS`, `AUTOEXEC.BAT`, and `.BAT` execution flow.

### Filesystem and compatibility
1. FAT-backed file handling is active.
2. FAT32 capability has advanced with mount metadata, hint-based allocation, dynamic directory growth, and edge-semantics gates.
3. INT21 priority-A compatibility is established and covered by dedicated regression gates.

### Video and desktop
1. The GOP/video stack supports mode selection, policy checks, and dynamic backbuffer management.
2. The desktop/UI path has layout hardening, overlay support, and non-interactive regression coverage.
3. Persistent video-mode configuration is supported through CMOS-backed boot configuration plus `VMODE.CFG` fallback.

### FreeDOS and optional payloads
1. A symbiotic FreeDOS runtime pipeline exists and is validated.
2. OpenGEM integration is optional and covered by dedicated tests.

### M6 protected-mode and DOS extender readiness
1. PMODE startup and transition contracts are instrumented and tested.
2. Protected-mode memory-domain overlap checks are in place.
3. The current smoke chain is:
   - `CIUKPM.EXE`
   - `CIUK4GW.EXE`
   - `CIUKDPM.EXE`
   - `CIUK31.EXE`
   - `CIUK306.EXE`
   - `CIUKLDT.EXE`
   - `CIUKMEM.EXE`
4. Current shallow DPMI coverage includes:
   - `INT 2Fh AX=1687h` host detection plus descriptor metadata
   - `INT 31h AX=0400h` get-version callable slice
   - `INT 31h AX=0306h` raw mode-switch bootstrap slice
   - `INT 31h AX=0000h` allocate-LDT-descriptors callable slice
   - `INT 31h AX=0501h` allocate-memory-block callable slice

### DOOM milestone baseline
1. The frozen first target is user-supplied shareware `DOOM.EXE` v1.9.
2. The primary IWAD expectation is user-supplied `DOOM1.WAD`.
3. `DOOM.WAD` is accepted only as a controlled alias to the same shareware dataset.
4. Expected runtime class is `DOS/4GW`-style.
5. Expected first video path is `VGA mode 13h`.
6. First milestone success checkpoint is main menu reachable.
7. Deterministic packaging/discovery baseline exists for `DOOM.EXE`, `DOOM1.WAD`, optional `DEFAULT.CFG`, and generated `DOOM.BAT` under `/EFI/CiukiOS`.
8. A staged boot-to-DOOM failure-taxonomy harness (`make test-doom-boot-harness`) classifies progress into `binary_found`, `wad_found`, `extender_init`, `video_init`, and `menu_reached` stages.
9. A VGA mode 13h compatibility scaffold is in place (shell `vga13` command + deterministic startup marker + `make test-vga13-baseline`) as the first step toward the real mode-13h draw path.
10. BIOS compatibility surface markers are emitted at boot for `INT 10h`, `INT 16h`, `INT 1Ah`, and `INT 2Fh` to make DOOM-startup dependencies greppable.

## Important Repository Files

### Root files
1. `README.md`: public-facing project overview and latest two changelog entries.
2. `CHANGELOG.md`: complete release/change history.
3. `Roadmap.md`: high-level roadmap and current milestone state.
4. `documentation.md`: central durable documentation for current project state.
5. `CLAUDE.md`: collaboration workflow and session rules.
6. `DONATIONS.md`: support and donation details.

### Core source areas
1. `boot/uefi-loader/`: UEFI loader.
2. `stage2/`: main runtime, shell, FAT, UI, video, and compatibility code.
3. `kernel/`: fallback kernel path.
4. `com/`: DOS smoke binaries and focused runtime probes.
5. `scripts/`: validation, orchestration, and packaging test scripts.
6. `third_party/`: imported external runtime payloads and related assets.

### Documentation areas
1. `docs/roadmap-ciukios-doom.md`: detailed DOOM-oriented execution plan.
2. `docs/roadmap-dos62-compat.md`: DOS 6.2 compatibility track.
3. `docs/int21-priority-a.md`: INT21 compatibility baseline and matrix.
4. `docs/handoffs/`: dated change handoffs for major tasks.

## Test and Validation Overview
The project emphasizes deterministic, scriptable validation.

Important gates include:
1. `make test-stage2`
2. `make test-fallback`
3. `make test-phase2`
4. `make test-video-mode`
5. `make test-video-1024`
6. `make test-video-ui-v2`
7. `make test-m6-pmode`
8. `make test-m6-smoke`
9. `make test-m6-dos4gw-smoke`
10. `make test-m6-dpmi-smoke`
11. `make test-m6-dpmi-call-smoke`
12. `make test-m6-dpmi-bootstrap-smoke`
13. `make test-m6-dpmi-ldt-smoke`
14. `make test-m6-dpmi-mem-smoke`
15. `make test-vga13-baseline`
16. `make test-doom-target-packaging`
17. `make test-doom-boot-harness`
18. `bash scripts/test_doom_readiness_m6.sh`

## Current Milestone Gaps
The main remaining gaps before the first DOOM milestone are:
1. Replace the current shallow bootstrap smoke ceiling with a more realistic DOS extender execution target.
2. Harden high-memory/protected-mode handoff for real extender behavior.
3. Close the remaining BIOS and DOS runtime gaps actually used by target startup.
4. Implement `VGA mode 13h` compatibility sufficient for a real frame/menu checkpoint.
5. Extend the current packaging baseline into a staged boot-to-DOOM runtime harness.
6. Add gameplay-relevant input and audio compatibility.

## Documentation Workflow
When a meaningful task is completed:
1. Update the relevant roadmap/changelog files if public status changed.
2. Write a handoff in `docs/handoffs/` for major multi-file or architectural changes.
3. Update this file if the task changed the stable project state, architecture, tests, or milestone progress.

This keeps three layers aligned:
1. `CHANGELOG.md` for release history.
2. `documentation.md` for current durable project state.
3. `docs/handoffs/` for detailed implementation history.