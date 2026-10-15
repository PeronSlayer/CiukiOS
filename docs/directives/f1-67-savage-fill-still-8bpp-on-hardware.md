# f1-67 — Savage engine still quarantined on the T23 after f1-65 (image a4f67ac1)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f1-67` from `main`. References on disk: `build/downloads/savage/` (X.Org driver: `savage_bci.h`, `savage_accel.c`, `savage_regs.h`, `savage_driver.h`; Linux framebuffer driver: `savagefb_driver.c`, `savagefb.h`, `savagefb_accel.c`, GPL: read for register semantics only, never copy).

## Evidence

Serial capture of boot 1 of image `a4f67ac1` (fragment, the records of that boot are interleaved): `[accel] sav bci_test result=failed error=-5` — the marker fill still fails after f1-65 sent the global bitmap descriptor through BCI set-register packets (`src/kernel/drivers/savage2d.c`, commit 5c22259a). The last boot's disk log contains no `[accel]` lines at all (check whether f0-22 moved the init records behind the diagnostic flag; the init block must always be logged). f0-23 adds log rotation so the next T23 log keeps every boot.

## Task

1. Compare the kernel's full engine start sequence against both references for the SuperSavage/IX (`S3_SUPERSAVAGE`): `SavageSetup2DEngine` / `SavageInitialize2DEngine` (CR31, CR40, CR50 bits 4–5 for 16/32 bpp, CR66 shadow, MM8128, MM48C18 BCI enable, the descriptor registers and their BCI equivalents), the order of operations relative to the mode set by the firmware, the required idle waits, and the framebuffer pitch/tiling assumptions (the desktop runs 1024×768×32, pitch 4096 from the VBE mode: check whether the engine needs the stride in pixels or in 8-byte units for this chip, and whether the "BW disable" bit must be clear).
   Concrete discrepancy to test first: the Linux framebuffer driver's `SavageSetup2DEngine` (`savagefb_driver.c:302–345`) builds the global bitmap descriptor as `1 | 8 | BCI_BD_BW_DISABLE | bpp<<16 | stride` for every chip, SuperSavage included (bit 3 set), while the kernel writes `0x10200401` (bit 3 clear, following the X.Org note that bit 3 is the Savage4 BCI-enable); it also programs `MM48C18` per chip (SuperSavage: `& 0x3FF0` then `| 0x08`) and writes the descriptor to `MM8168`/`MM816C` after the BCI setup. Reproduce that exact order and value in the register model, then on the T23.
2. Make the marker test diagnose, not only fail: on failure record the readback of every engine register it wrote (CR31, CR40, CR50, MM8128, MM48C18, both descriptor registers and the BCI status) so that the next T23 log identifies the mismatch.
3. If the 32 bpp fill cannot be made to work from the available documentation, implement and test the 16 bpp path as an alternative (the desktop at 1024×768×16 on the T23 with a correct engine beats 32 bpp in software), selectable from the display settings, and say so.

## Acceptance

Kernel host suite green (register model extended); next T23 log: `bci_test result=ok` and `caps` non-zero, or the full readback that explains the failure.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary. Host tests one at a time with `nice -n 19`, `rg` not semble. Plain technical prose, no names of other operating systems. Report `build/codex/f1-67-report.md`.
