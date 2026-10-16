# f3-37 — more native full-screen modes and colour depths on the SuperSavage

- **Phase:** F3. **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f3-37` after f3-36 is merged.
- **Authority:** `src/kernel/drivers/supersavage_display.c` (mode table and programming), `src/kernel/vm/dosvm_vga*.c` (guest mode requests), `docs/design/` DOS video record, host tests.

## Owner request (2026-10-16, T23, image a79f840c)

Native full screen works for Quake (mode 13h, 320x200x8). The owner wants more resolutions and colour depths recognised in full screen. Today the provider accepts a fixed list; guests asking for other VGA modes (mode X variants, 320x240, 360x240, 640x400, 640x480x16 colours) or VBE modes (640x480, 800x600 at 8/15/16 bpp, used by DOS games through VESA) are refused and stay windowed.

## Required

1. Inventory: from the X.Org Savage driver's mode programming and the IBM INF's mode list, enumerate the modes the SuperSavage IX on the 1024x768 panel can scan out natively (CRTC timings, panel expansion/centering), and which guest VGA/VBE mode numbers map onto them; record the table in the design note with the register sources.
2. Implement the additional VGA modes first (all planar/chain-4 mode-X variants and the 640x480x16 planar modes the VGA core already models), then the VBE linear modes 640x480 and 800x600 at 8 and 16 bpp through the existing guest VBE interface (the f3-31 record says guest VBE is currently rejected: lift that only for the modes the provider supports, with the same save/restore transaction and the f3-36 fallback).
3. Each new mode: enter, first guest frame, exit and desktop restore on the host model; the DOS window path unchanged; QEMU vm-fullscreen cases for the new modes. Run the host suites and builds as usual. Report in `build/codex/f3-37-report.md`.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits. No register programming that is not in the X.Org/savagefb sources or the INF.
