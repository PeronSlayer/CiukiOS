# f0-26 — Hardware self-check page at boot (L menu) with a Savage engine configuration sweep (owner request 2026-10-15: stop circling the same defects)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f0-26` from `main`. Build the kernel before reporting.

## Why

Every T23 round so far needed a new image to learn one fact (Savage fill still 8 bpp after two fixes; stick quarantined; mouse report layout; timer gaps). The owner asked for a definitive method. The kernel must test the hardware itself in one boot and show per-item results on screen and in the log, so one boot answers every open question and a fix can be verified without interpretation.

## Task

1. A `Hardware check` entry in the boot menu (beside L) that boots the kernel into a text page instead of the desktop and runs, in order, with PASS/FAIL and the key numbers on screen (and the same lines in the disk log and the black box): ATA disk write+readback in a scratch file, forced-unclean-shutdown simulation (write, no publish) followed by an in-kernel mount repair check; USB stick enumerate+mount+read 1 MiB with elapsed ms; USB mouse: descriptor parse summary (buttons, axes, wheel) and a live 5 s window showing decoded button/axis/wheel events as they arrive; PS/2 keyboard and the EC volume keys live; timer: ticks lost over 10 s with interrupts-off max and its attributed section; serial: a 100-record burst with a counter of whole records sent; Savage: the engine configuration sweep (item 2); audio: codec ready and a 1 s tone.
2. **Savage sweep** (`src/kernel/drivers/savage2d.c` test mode only): for each candidate configuration run the marker fill and record the readback: descriptor bit 3 on/off; `BW_DISABLE` on/off; stride field in pixels vs qwords; CR50 depth bits 00/01/11; tiled destination bits 16/32 vs linear; 16 bpp mode (1024×768×16 via the display service) with the same matrix; MM8136 (which does not retain its value in the T23 log: `wrote=00000007 read=00000000`) and CR45 (`wrote=00 read=c0`) explained from the references in `build/downloads/savage/`. Print the first configuration whose fill reads back exactly, or "none"; the production driver then adopts that configuration from a setting the lead can change without a rebuild (registry key), defaulting to the current one.
3. Everything on the page is bounded (no step longer than 15 s), skips cleanly when a device is absent, and never leaves the hardware in a changed state (restore VGA/engine after the sweep).

## Acceptance

Kernel host suite green with the page's test model; QEMU boots the page and prints PASS for the emulated items; on the T23 the owner reads the page and photographs it.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary; host tests one at a time with `nice -n 19`. Report `build/codex/f0-26-report.md`.
