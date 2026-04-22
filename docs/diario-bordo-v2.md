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
13. Milestone: added deterministic Stage1 boot selftest path with serial PASS markers for DOS and COM runtime checkpoints.
14. Milestone: introduced `qemu-test-stage1` automated regression and integrated it into `qemu-test-all`.
15. Release: bumped project version to `CiukiOS pre-Alpha v0.5.5` after Stage1 milestone closure.
16. Next step: move from embedded COM payload to FAT-backed `.COM` file loading and execution.
17. Milestone: added `.EXE` MZ runtime baseline in Stage1 with relocation handling and PSP-linked execution path.
18. Milestone: added deterministic `mzdemo` command and serial regression markers integrated in `qemu-test-stage1`.
19. Milestone: added Stage1 `INT 21h` handle-based file I/O baseline (`open/read/close/seek`) with deterministic `fileio` regression marker.
20. Action: expanded Stage1 floppy slot from 6 to 14 sectors to keep Phase 2 runtime growth stable.
21. Milestone: added Stage1 DOS execute baseline (`INT 21h AH=4Bh`) for path-driven `.COM/.EXE` launch.
22. Milestone: switched Stage1 `comdemo` and `mzdemo` validation paths to use DOS `AH=4Bh` execution flow.
23. Milestone: added minimal MCB-compatible header behavior to Stage1 memory services (`INT 21h AH=48h/49h/4Ah`).
24. Milestone: added Stage1 DTA and file search baseline (`INT 21h AH=1Ah/4Eh/4Fh`) with deterministic `findtest` coverage.
25. Milestone: added Stage1 drive/CWD compatibility baseline (`INT 21h AH=19h/0Eh/3Bh/47h`).
26. Milestone: refreshed the Stage1 shell presentation with a boot splash, loading bar, top banner, clearer help surface, and explicit logical root layout.
27. Milestone: kicked off Phase 3 with a Stage1 VGA mode 13h graphics demo plus deterministic `GFX-SERIAL` regression marker coverage.
28. Milestone: extracted reusable Stage1 VGA mode 13h drawing primitives for pixels, horizontal/vertical lines, rectangles, generic line drawing, and bitmap text.
29. Milestone: upgraded the Stage1 graphics smoke path to a non-blocking timer/input-driven demo loop instead of a fixed blocking frame hold.
30. Milestone: introduced a small VDI-like video compatibility layer (`vdi_enter_graphics`, `vdi_bar`, `vdi_box`, `vdi_line`, `vdi_gtext`) as the first bridge toward the OpenGEM rendering path.
31. Action: expanded the Stage1 reserved loader budget from 18 to 20 sectors to keep the new graphics baseline stable in both floppy and full profiles.
