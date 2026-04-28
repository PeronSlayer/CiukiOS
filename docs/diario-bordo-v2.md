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
30. Milestone: introduced a small VDI-like video compatibility layer (`vdi_enter_graphics`, `vdi_bar`, `vdi_box`, `vdi_line`, `vdi_gtext`) as the first bridge toward the desktop runtime rendering path.
31. Action: expanded the Stage1 reserved loader budget from 18 to 20 sectors to keep the new graphics baseline stable in both floppy and full profiles.
32. Milestone: implemented FAT16 multi-sector cluster semantics in Stage1 DOS runtime core paths (`open/read/write/exec`) and fixed root-file load cluster-to-LBA conversion for Stage2 chainload.
33. Milestone: aligned full-profile FAT16 image build with 8-sector cluster geometry, including coherent FAT entries and cluster-based payload placement.
34. Milestone: expanded DOS compatibility with additional `INT 21h` services (`2Ah`, `2Ch`, `33h`, `34h`, `36h`, `52h`, `54h`, `58h`) and minimal `INT 2Fh` multiplex handler.
35. Milestone: corrected `INT 21h` register return behavior for `ES:BX`-based APIs and added serial diagnostics for unsupported DOS calls (`[INT21-UNSUP]`).
36. Milestone: integrated desktop runtime payload in both root and legacy nested layouts in the full image and hardened Stage2 launcher with deterministic trace markers.
37. Action: introduced guarded desktop runtime safe-launch mode (default) to prevent shell hard-hang while preserving an opt-in real execution path for compatibility testing.
38. Action: expanded Stage1 reserved loader budget from 21 to 22 sectors in both full and floppy profiles to absorb DOS runtime completion work.
39. Release: bumped project version to `CiukiOS pre-Alpha v0.5.7` after DOS runtime compatibility expansion and regression validation.
40. Milestone: fixed desktop runtime nested execution path (`display bootstrap -> desktop executable`) with separated MZ load segments, restored parent PSP context after child return, and corrected FAT short-name propagation for DOS find-first compatibility.
41. Milestone: extended DOS heap limit and introduced two-block allocation behavior to improve GEM runtime memory allocation during launch sequence.
42. Action: expanded Stage1 reserved loader budget from 22 to 23 sectors in both full and floppy profiles to keep runtime growth stable.
43. Release: bumped project version to `CiukiOS pre-Alpha v0.5.8` after desktop runtime stabilization work and regression validation.

## 2026-04-23 (continued)
44. Action: restored descriptive shell messages in Stage1 (banner with version, full help listing, readable prompt and error text) after text regression introduced by stage2 overflow fix.
45. Action: expanded Stage1 reserved loader budget from 23 to 24 sectors in both full and floppy profiles, bringing both in sync; floppy was silently overflowing by 330 bytes.
46. Fix: corrected `INT 21h AH=49h` (free) block2 path — `.free_done` was aliased to `.invalid`, causing all block2 free calls to return error 9 instead of success.
47. Fix: added block2 path to `INT 21h AH=4Ah` (resize) — previously only block1 was handled, leaving GEM runtime with inconsistent memory state on resize calls targeting the secondary heap block.
48. Action: created `setup/` installer skeleton — source stubs, build scripts, manifest, and full TODO list for a future DOS-Setup-style multi-floppy + CD-ROM installer project.
49. Fix: improved DOS MCB chain consistency for two-block allocations: block2 now writes a physical MCB header, block1 MCB toggles between `M` and `Z` when block2 is allocated/freed, and `INT 21h AH=52h` now returns a valid first-MCB pointer via List-of-Lists.
50. Fix: upgraded DOS keyboard input services from stubs to BIOS-backed behavior (`INT 21h AH=06h/07h/08h/0Ah` using `INT 16h`), removing forced-CR behavior that could destabilize interactive runtime workloads.
51. Fix: implemented `INT 21h AH=51h` and corrected PSP reporting for `AH=62h` to use `current_psp_seg` instead of stale MZ-only context.
52. Action: enabled full-profile real desktop runtime path by default (`CIUKIOS_STAGE2_AUTORUN=1`) and added explicit Stage2 autorun diagnostics to support deterministic runtime tracing.
53. Release: bumped project version to `CiukiOS pre-Alpha v0.5.9` after desktop runtime probe-path stabilization (carry preservation in DOS I/O tails and special find-next compatibility adjustments).

## 2026-04-24 — Desktop runtime milestone closure (v0.5.9-final)
54. Milestone: completed final gate — 20 QEMU sessions; 100% launch, 100% return-to-shell, 0 hangs → **PASS**.
55. Milestone: completed OG-P1-01 — hardened Stage2 VDI/AES primitives (coordinate clipping, stateful INT33h); added offline validation module for graphics runtime.
56. Milestone: completed OG-P1-02 — long-session soak harness for the full profile; 100 runs × 20 min → **PASS** (0 failures, 0 errors).
57. Milestone: completed hardware lane on physical x86 PC (HP w19 monitor); markers `[HW] PASS: desktop autorun completed` and `[HW] PASS: returned to CiukiDOS shell` observed; evidence committed in `docs/hardware/`.
58. Milestone: completed runtime normalization and launch-order troubleshooting guide.
59. Milestone: completed regression lock with 10 deterministic checks (carry, find-next, alias, memory free/resize, autorun markers) → **PASS** 10/10.
60. Milestone: completed performance baseline + budget framework → **PASS**.
61. Milestone: final validation bundle aggregates gate, acceptance, soak, and hardware evidence; **Verdict: PASS** (label: final-closure-ready).
62. Action: added failure-classification counters to acceptance and soak scripts (`launch_without_return`, `qemu_fail`, `infra_fail`, `unexpected_exit`) for diagnostic precision.
63. Action: added CD-ROM profile scaffolding (`scripts/build_full_cd.sh`, `src/boot/full_cd_mbr.asm`) as foundation for the next distribution milestone.
64. Action: workspace cleanup — removed intermediate QEMU debug logs (~80 files), ~14 stale test-run directories, agent session directories (`Agents/`, `setup/`), debug binary dumps, and obsolete root markdown files.
65. Release: bumped project version to `CiukiOS pre-Alpha v0.5.9-final`; all P0/P1/P2 desktop runtime validation gates closed including real hardware evidence.
