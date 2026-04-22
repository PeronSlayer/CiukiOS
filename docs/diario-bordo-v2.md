# Engineering Logbook - CiukiOS Legacy v2

## 2026-04-22
1. Decision: full architectural reset toward legacy BIOS x86.
2. Action: previous project archived to `OLD/archive-2026-04-22/`.
3. Action: new documentation baseline created (architecture, roadmap, AI directives).
4. Action: versioning baseline reset to `pre-Alpha v0.5.0`.
5. Milestone: delivered a BIOS-bootable `floppy` stage0 baseline with a real 16-bit boot sector.
6. Milestone: upgraded floppy QEMU smoke testing to assert a concrete boot marker.
7. Action: defined `DOS Core Specification v0.1` and `DOS Core Implementation Plan v0.1` as the normative project baseline.
8. Milestone: implemented `stage0 -> stage1` chain loading with deterministic sector layout.
9. Milestone: implemented a real-mode Stage1 BIOS monitor with diagnostics for `INT 10h/13h/16h/1Ah`.
10. Milestone: added a minimal Stage1 command loop for runtime bring-up checks (`help`, `cls`, `ticks`, `drive`, `reboot`, `halt`).
11. Milestone: initialized `INT 21h` vector in Stage1 and implemented baseline DOS services (`AH=02h`, `AH=09h`, `AH=4Ch`, `AH=4Dh`) with smoke command `dos21`.
12. Milestone: added first `.COM` execution baseline with PSP-compatible setup and deterministic command `comdemo`.
13. Next step: move from embedded COM payload to FAT-backed `.COM` file loading and execution.
