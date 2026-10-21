# f1-88 — Savage BCI quarantine on every T23 image: root cause against the X.org and Linux Savage drivers, fix candidates with host model tests, T23 experiment protocol

- **Owner:** Codex, `gpt-6.1-sol`, effort `xhigh`, `--search`
- **Written:** 2026-10-21 by the lead. On every T23 image since f1-65/f1-69 the accelerator driver quarantines itself at boot and the desktop composes in software, which costs CPU on every window move (owner: moving a window makes the music lag). Records of the 2026-10-21 trial (image b51f034f): `[accel] savage pci=5333:8c2e mmio=c0100000 lfb=e8000000 mode=280 bpp=32 pitch=4096 caps=0 init_result=-5`; `[accel] savage quarantined reason=bci_test_readback_failed us=243052 reset_us=177346 poll_us=14 trace_us=21775 status=07e00000 status_valid=1`; `[accel] savage EngineConfig=1`; `[accel] savage phase=marker_write us=40731 error=0`; the `savage init`/`savage readback` register records with banks `cr`, `sr`, `mmio`, `bci`, `port` are in the trial logs (the lead supplies the exact lines on request through an amendment; the record format is in `src/kernel/drivers/savage2d.c`). The device is the SuperSavage IX/C of the ThinkPad T23 (PCI 5333:8c2e).
- **Worktree:** `../CiukiOS-wt-f1-88`, branch `wt/f1-88`
- **Sources on disk:** `build/downloads/savage/` holds `savage_driver.c/.h`, `savage_accel.c`, `savage_bci.h`, `savage_regs.h`, `savage_cursor.c`, `savage_dga.c` (xf86-video-savage) and `savagefb_driver.c`, `savagefb_accel.c`, `savagefb.h` (Linux savagefb); cite upstream URLs and the S3 documentation you find; `docs/directives/f1-65-*.md`, `f1-69-*.md`, `f3-32-*.md`, `f3-43-*.md` and `docs/validation/` Savage records are our history.

## Required

1. **Root cause.** Compare our initialisation and BCI self-test in `src/kernel/drivers/savage2d.c` (and `supersavage_*.c` where it touches the engine) step by step with `SavageInitialize2DEngine`, `SavageSetGBD`, the SuperSavage-specific paths (`S3_SUPERSAVAGE` chip handling: BCI enable bits in `0x8504`/`0x8508`, the alternate status register and FIFO size, `bciUsedMask`, the `WaitQueue`/`WaitIdleEmpty` register choice for this chip family, the global bitmap descriptor requirements for 32 bpp and pitch 4096, the `MAXFIFO`/COB settings, the shadow status/`ShadowStatus` option, the MMIO window layout at 0x10000 for BCI) and with savagefb's `savage_init_hw`/`savagefb_set_par` and its 2D acceleration init. Decode `status=07e00000` with the status-word layout the drivers use for this family and say what it tells (idle bits, FIFO free entries, the BCI event tag). Determine why our marker readback fails after a successful `marker_write`: wrong status register for the chip, missing BCI enable or COB setup, wrong event-tag semantics, the engine still in the reset we start, or the 177 ms reset path leaving it disabled.
2. **Fix candidates.** Implement the corrected sequence behind the existing self-test, with a host register model that reproduces the SuperSavage behaviour from the upstream sources (the model is the acceptance: our old sequence fails on it the way the T23 does, the corrected one passes), keeping the quarantine as the safety net. No change to the mode-set path (f3-32) beyond what the engine init requires, documented.
3. **T23 experiment protocol** in `docs/validation/f1-88-savage-bci.md`: the exact records the lead must read after the next trial to confirm or refute (init sequence trace, status decoding, marker readback), and a `BOOT.CFG` token to force software composition (`accel=0`) so the owner always has a safe boot.

Verification: `tests/host` modules of the accelerator and display green one at a time with `nice -n 19`; `scripts/test/host_kernel_tests.sh` as far as the sandbox allows. Report `build/codex/f1-88-report.md` with the comparison table (our step, upstream step, difference, consequence), citations, decisions needed. `rg`/`sed` only; no QEMU, no `systemd-run`, no image builds, no commits, no diary edits; no new `panic()` paths; English.

## Amendment 1 (lead, 2026-10-21): the T23 register records, and what they refute

The complete `[accel]`/`[display]` records of both boots of the b51f034f trial are in `build/t23-b51f034f-accel-records.txt` of this worktree (two boot logs concatenated; the first block is the later boot). Key lines:

```
[accel] savage engine_probe reg=48c18 raw=00000008
[accel] savage status reg=48c60 raw=07e00000
[accel] savage idle mask=00e1ffff value=00e00000
[accel] savage descriptor wrote=10200401 read=10200401 mask=13ff1ffd cr40=01
[accel] savage bci_test phase=fill x=1023 y=767 wrote=00cd00e8 before=0097a5b2 cmd=4bcc8000
[accel] savage bci_test result=failed error=-5 wrote=00cd00e8 read=e8e8e8e8 read_valid=1 restored=1 status=07e00000 status_valid=1
[accel] savage init reg=08134 wrote=00000027 read=00000027 bank=mmio width=16
[accel] savage init reg=08136 wrote=00000007 read=00000000 bank=mmio width=16
[accel] savage init reg=0816c wrote=10200401 read=10200401 bank=mmio width=32
```

Lead reading, binding for the next session: the engine **did** execute the fill and completed it (status idle, the pixel changed from `0097a5b2`), but the pixel reads `e8e8e8e8`: the low byte of the requested colour `00cd00e8` replicated into all four bytes. That is the 8-bpp replication f1-65 already modelled on the host, not a stale completion. Your completion-fence candidate therefore does not address the T23 failure (keep it only if it is harmless and documented; otherwise drop it). Investigate instead:

1. the destination depth the engine actually uses for the fill: the global bitmap descriptor `0x10200401` as written through `0x816c` (and its second descriptor word `0x8168 = 0`), versus `SavageSetGBD`/`SavageSetGBD_Twister`/`SavageSetGBD_PM` for the SuperSavage family in `savage_accel.c` (bpp field, stride units, `BCI_BD_BW_DISABLE`, tile bits, and the `0x8168`/`0x816c` versus BCI-register path), including the required write of the descriptor through BCI `SET_REGISTER` after a mode set and the `cr50`/`cr31` pixel-depth programming for 32 bpp (`cr50 = f1` was written: check its meaning for 32 bpp);
2. the fill command `4bcc8000` against `BCI_CMD_RECT | BCI_CMD_RECT_XP | BCI_CMD_RECT_YP | BCI_CMD_SEND_COLOR | BCI_CMD_DEST_GBD | BCI_CMD_SRC_SOLID` and the colour operand format for 32 bpp (does the SuperSavage expect the colour in the pattern register, and does `SEND_COLOR` with an 8-bpp destination descriptor replicate the low byte exactly as observed?);
3. the unexplained `0x8136 wrote=0007 read=0000` (16-bit write at an odd offset into a 32-bit register `0x8134`? check the register's width and meaning in `savage_regs.h`) and whether that register carries the destination format;
4. the `caps=0` and `EngineConfig=1` records: what the driver concluded about the chip family (SuperSavage IX/C is `S3_SUPERSAVAGE`, with the `0x48c60` alternate status and a 32-entry FIFO without COB: confirm against `savage_driver.c`).

Deliver the corrected descriptor/depth programming behind the same self-test, the host register model reproducing `e8e8e8e8` with the old programming and the exact colour with the new one, and update the T23 protocol accordingly. Report section "Amendment 1".
