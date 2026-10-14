# f4-19 — Savage quarantined on the T23: `descriptor_readback_failed` (SuperSavage IX/C 2D engine initialisation)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f4-19` from `main`. Host-testable (`tests/host/savage2d_test.c` model); the lead tests on the T23 (QEMU has no Savage).

## Evidence (T23, image fb3147dc, clean disk log `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/fb3147dc/disk-log/BOOT.LOG.clean`, lines 167-423)

`[accel] savage probe pci=5333:8c2e cmd=0007 bar0=c0100000 bar1=e8000008 lfb=e8000000 vram=15532032`; `mapped mmio=f861c000 cursor=f869d000`; `vga_alias reg=083d5 port=68 alias=49 match=0` (the MMIO alias of CR data does not reflect the port); `engine_probe reg=48c18 raw=00000008`; `status reg=48c60 raw=07e00000`, `idle mask=00e1ffff value=00e00000` (Savage4-series idle predicate satisfied); **`quarantined reason=descriptor_readback_failed us=202256 reset_us=21528 poll_us=5 trace_us=702 status=07e00000 status_valid=1`**, `init_result=-5`, `fallbacks=1`. `src/kernel/drivers/savage2d.c:376-391`: after CR40|=1 (:324-326), the pipe select check on SR26 (:337), CR31 = (saved & ~1) | 0x0c, plane masks 0x8128/0x812c = ~0, 0x8134 = 0x00070027, 0x48c0c = 0, 0x48c10 = 0x00700040, GBD1 0x8168 = 0, GBD2 0x816c = descriptor | 1 (SuperSavage) and CR50, the check `read32(0x816c) != gbd || !(CR40 & 1)` fails. The 0x48Cxx block answers plausibly while the 0x8xxx 2D block (and the 0x83xx VGA alias) does not: the 2D register window is not enabled, or the GBD2 format for the SuperSavage differs, or CR40 bit 0 does not stick.

## References (fetch and compare register by register)

xf86-video-savage `savage_driver.c` (`SavageEnableMMIO`: VGA enable 3C3 bit 0, 3C2, CR38/CR39 unlocks, CR40 bit 0; `SavageInitialize2DEngine` for `S3_SUPERSAVAGE`: CR40 = 0x0140, CR31 = 0x0c, `0x48C18 &= 0x3FF0`, `0x48C14` = COB offset/index, `0x48C18 |= 0x0C`, plane masks), `savage_accel.c` (`SavageSetGBD_PM`/`SavageSetGBD_M7`/`SavageSetGBD_Twister`: which one the SuperSavage uses, the GBD1/GBD2 bit layout — BCI enable, 64-bit, little-endian, block-write disable bit 28 — and the primary stream registers 0x81C0/0x81C4/0x81C8), `savage_regs.h`; Linux `drivers/video/fbdev/savage/savagefb_driver.c` (`savage_enable_mmio`, `SavageSetup2DEngine`, `savage_set_default_par`, `savagefb_set_par_int`), `savagefb.h`; S3 SuperSavage/Savage4 register reference (RE046-B is cited in our code: CR53 bits 3/4 "MMIO enable", CR58 linear addressing, CR66/CR68 lock bits).

## Task

1. Make the SuperSavage 2D initialisation follow the two open-source drivers exactly (order, registers, bit values), including whatever enables the 0x8xxx register window (CR53? 3C3? CR40 sequence) and the correct GBD2 format for chip 0x8c2e; keep the quarantine/fallback and the idle predicate.
2. Per-register readback records at `klog` level for the first initialisation: `[accel] savage init reg=... wrote=... read=...` for every register the init writes, plus the BCI test (marker fill, status, readback) outcome, so the next T23 log is conclusive even on failure.
3. Never touch CRTC timing, DAC or stream registers that could blank the panel; only the 2D engine/BCI path the references use for acceleration.
4. Host model (`savage2d_test.c`): a SuperSavage model whose 0x8xxx window is dead until the enabling sequence runs, and whose GBD2 readback masks reserved bits; tests for the sequence and for the records.

## Acceptance

Host suites green; the lead tests on the T23: `[accel] driver=savage ... quarantined=0 ops>0`, desktop fills/blits accelerated with the Ciuki portrait rendering correctly; otherwise a conclusive record. Files: `src/kernel/drivers/savage2d.c`, `fbdev.c` hooks, `tests/host/savage2d_test.c`.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time (the workstation is short of memory). Research the references on the internet first (`--search` is enabled), record the links and the decision in the relevant `docs/design/` or `docs/validation/` document, then implement and test. Report `build/codex/f4-19-report.md`: what the T23 evidence proved, what changed, which host tests prove it, what the lead must verify on QEMU and on the T23.
