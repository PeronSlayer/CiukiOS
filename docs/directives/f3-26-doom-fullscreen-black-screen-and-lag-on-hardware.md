# f3-26 — DOOM on the T23: full screen gives a black screen with sound and locks the machine; windowed DOOM still lags (image a4f67ac1)

- **Model/effort:** `gpt-6.1-sol`, `xhigh` (Astra only if Sol fails twice). **Worktree:** `../CiukiOS-wt-f3-26` from `main`.

## Evidence

- Owner: DOOM loads faster than on `a738204a` (f3-24 disk windows) but still lags in the window; Alt+Enter full screen: the screen goes black, the game's sounds continue in the background, the machine must be powered off. No `[dosvm] fullscreen` or `[display] fullscreen` record survives in the serial capture of that boot, and the disk log holds only the last boot (f0-23 adds rotation).
- The full-screen path (f3-20) saves and restores the VGA/VBE state through the BIOS VM and sets the game's mode with the card's own video BIOS running in V86 on the real SuperSavage; on QEMU (std VGA) the same path entered mode 19 and showed the demo (image `94542c30`). The real BIOS may touch MMIO, PCI configuration or ports that the V86 session is not granted, or the Savage's streams/LCD panel path may need the BIOS's own mode-set sequence (the panel stays black when the stream processor is left in the desktop mode).

## Task

1. Record every step of the full-screen transaction on hardware in the disk log before it happens (grant, VGA save, BIOS call with the function and the registers, port faults with the port number, the first guest frame, the watchdog): the next T23 attempt must leave a readable trail even if the machine locks.
2. From the SuperSavage reference (`build/downloads/savage/savage_driver.h`, `savage_accel.c`: streams, `SavageSave/Restore`, panel/LCD and CR/SR registers; `docs/design/savage-2d.md`) decide what the mode switch needs on this card that the BIOS VM path does not provide (stream processor off for VGA modes, CR67/CR69 stream control, panel expansion, the VGA memory aperture), implement it in the display/full-screen path, with a watchdog that restores the desktop mode on any fault instead of leaving the screen black.
3. Windowed DOOM lag: with the f3-19 fast path and f3-23 fix in place, use the per-second `vm_perf_*` records (now behind the diagnostic flag; enable them for one T23 boot through the screen-log menu option) to attribute the remaining cost (port traps, page remaps, presenter, audio) and fix the dominant one.

## Acceptance

Kernel host suite green (full-screen service and VGA transport tests extended); QEMU `vm-fullscreen-qemu-t23` and `vm-fullscreen-cirrus-qemu-t23` with f3-22's timings; on the T23 the next attempt either shows DOOM full screen or returns to the desktop with a recorded reason.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary. Host tests one at a time with `nice -n 19`, `rg` not semble. Evidence (read-only): `legacy/local/physical/a4f67ac1/` (`disk-BOOT.LOG` = last boot only, clean; `serial.log` = three boots, interleaved fragments). Plain technical prose, no names of other operating systems, no filler. Report `build/codex/f3-26-report.md`: root cause (file:line), files changed, verbatim test output, what the lead must verify on QEMU and on the T23.
