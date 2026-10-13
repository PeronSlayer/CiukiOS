# f1-27 — Savage 2D engine initialisation stalls the T23 boot: make it correct, late and bounded, then enable it again

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search` (S3 Savage4/SuperSavage register documentation, X.Org `savage` driver as behavioural reference for the MMIO base per chip generation, no code copying). **Worktree:** `../CiukiOS-wt-f1-27` from `main`. Host-testable (engine model with a stuck clock and dead MMIO); the lead qualifies on the T23 with an `L` capture.

## Evidence (T23, 2026-10-13, image 8845b848 / hardware 62e4dc6c)

Both the `N` and `L` boots stop right after `[init] flag safe_mode=0 boot_flags=00000a08 activation_seq=1`; no `[init] framebuffer` line follows. `fbdev_init()` (src/kernel/drivers/fbdev.c ~175) called `savage2d_init(&device)` during the framebuffer activation; the lead made that call compile-time opt-in (`CIUKI_ACCEL_SAVAGE`, commit 03bf0427) as a stopgap. The owner wants the driver working, not disabled. The adapter is the SuperSavage IX/C, PCI 5333:8c2e (two `[display] mtrr wc` lines show the LFB at e8000000, 4 MiB WC range).

## Likely defects to verify and fix (`src/kernel/drivers/savage2d.c`, the fbdev glue)

1. **Waits that cannot end**: `wait_idle`/`sync_engine` bound their loops with `io->now_us`; at framebuffer activation the time source may not be calibrated yet (TSC/PIT), so a wait never expires. Bound every wait by an iteration count as well as time, and make a stuck clock a quarantine reason.
2. **MMIO base and BAR validation**: on Savage4/ProSavage/SuperSavage the MMIO registers live at the LFB BAR base + 0x1000000 (new-style MMIO), on Savage3D/MX elsewhere; the BAR must be large enough (the 0x8c2e aperture), the PCI command register must have memory space enabled, and the mapping must be uncached. Before any engine register is used, read a known register (status/BCI) and refuse (quarantine) on 0xFFFFFFFF or on values that contradict the VBE mode (pitch/bpp). Record the BAR, the computed MMIO base and the first reads in verbose boot.
3. **Register access order on real silicon**: CR66.0 (engine enable) read through the VGA CRTC ports requires the CRTC extended registers unlocked (CR38/CR39/CR40 S3 unlock sequence) on real chips; verify the unlock is done before reading/writing CR31/CR40/CR45/CR50/CR55/CR66; verify the BCI enable bits and the GBD/PBD descriptors against the documentation for the SuperSavage (tiled versus linear, BCI buffer size bits 0x48c18).
4. **Late initialisation**: do not initialise the engine inside `fbdev_init()`; do it from a kernel work item after timers are calibrated and the console is quiet (before the desktop's first present, or lazily at the first accelerated operation), under a watchdog: if the engine does not answer within 100 ms the driver quarantines itself and the software presenter continues. The boot must reach the desktop even when the chip does not answer at all.
5. **Records**: `[accel] savage bar=.. mmio=.. status=.. cr66=.. step=..` for each step in verbose boot, `[accel] savage ready` or `[accel] savage quarantined reason=..` always (one line), so a single `L` capture explains a failure.
6. Remove the compile-time gate once 1–5 are in place (acceleration on by default, with the quarantine as the safety net); keep an explicit `accel=0` BOOT.CFG key only if the loader already parses keys cheaply, otherwise leave it for later and say so.

## Tests (host)

Engine model variants: stuck clock, dead MMIO (0xFFFFFFFF), MMIO base at the wrong offset, locked CRTC registers, engine answering late; the boot path reaching the desktop with each; the records; existing `savage2d_test`, `fbdev_test` and presenter equivalence tests green.

## Constraints

clang i686 `-Werror`; no QEMU, no commits, no diary; `nice -n 19`, one host suite at a time; use `rg` not semble. Report `build/codex/f1-27-report.md` with the register references, the exact init sequence programmed and what the lead must read in the next `L` capture.
