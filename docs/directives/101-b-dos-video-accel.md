# 101-b — 1.0.1 alpha: DOS game video path and hardware acceleration

- **Written:** 2026-10-25 by the lead. Implementer: Codex gpt-6.1-sol, effort xhigh. Worktree `../CiukiOS-wt-101-dosvid`, branch `wt/101-dosvid`.
- **Context:** owner's trial on real hardware (Pentium III-M 1.13 GHz class laptop with an S3 SuperSavage-class 2D engine; image of main 60d029c9): "DOS games much better; hardware acceleration still needs work". DOOM, Wolfenstein 3-D and Quake run in a window and full screen. Read the existing work first: docs/design/display-driver-model.md, the DOS VM video/VGA model and window presentation (src/kernel/vm/dosvm_vga*, the dosbox client apps/dosbox, the native full-screen mode for DOS games, fx-27 windowed DOS graphics in docs/validation/fx-27-windowed-dos-graphics.md, the 2D engine use by the desktop), and state what the current per-frame path costs.

## Task

1. Measure the per-frame cost of a DOS game frame on the target model for mode 13h, Mode X (unchained 256-colour) and VESA LFB modes, windowed (with Fit/scale) and full screen: trap/emulation cost of VGA I/O (palette writes 3C8/3C9 — QEMU showed `perf_cost cause=vga_io exits=108879 us=334859` in a DOOM session), dirty tracking, palette conversion, scaling, copy into the desktop surface, compositor present.
2. Use hardware acceleration where the driver model provides it (2D engine blit/scale/colour-expand on the supported chips through the display driver model; the generic VESA path keeps a software fallback): scaled blits for windowed games, direct framebuffer for full screen, palette-change batching (no full reconversion per palette write), dirty-rectangle presentation. Follow the Windows 98 model (VDD virtualising VGA, DIB engine / display driver acceleration).
3. Host tests from the measured figures that fail before and pass after (per-frame cost of a mode 13h frame with a palette fade, a Mode X page flip, a VESA LFB frame; windowed scale through the accelerated and the software path produce identical pixels).

## Acceptance

Kernel and apps build without warnings; the DOS VM host tests (dosvm_test, tests/host/vm/run.sh, the VGA/fx-27 tests), the display driver tests and the desktop suites you touch pass. Report with before/after figures.
