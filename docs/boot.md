# Boot modes and what the screen shows

This page describes the boot of the canonical image (`build/f0/ciukios.img`)
on QEMU and on a PC. The loader is `src/boot/ciukldr.asm`
(`ciukldr/menu.inc`), the screen console `src/kernel/core/console.c`.
Contract changes: `docs/design/boot-memory.md` (amendment 2026-10-12) and
`docs/design/execution-abi.md` (system call 73).

## Normal boot

No fw_cfg test request, no `probe=` and no `safe=1` in `BOOT.CFG`, no key
pressed in the menu window.

1. Firmware POST.
2. The loader clears the text screen and shows one line at the bottom,
   `Press S for safe mode, L for serial log`, for about 3 s.
3. The loader sets the VBE mode (black screen) and loads `VMM.ELF`.
4. The kernel draws the boot splash, the original Ciuki photograph
   (`misc/CiukiOS_SplashScreen.png`), as soon as it maps the framebuffer,
   and then draws no log line. The splash stays until `/bin/desktop` takes
   the display (its display grant) and paints over it.
5. The desktop.

The splash is a deterministic format conversion done at kernel build time by
`scripts/convert_splash.py` (no Pillow): exact 2:1 nearest-neighbour
reduction to 512x384, RGB565, source and output SHA-256 pinned, embedded in
the kernel (384 KiB of read-only data). The console scales it with nearest
neighbour to the largest 4:3 area of the screen (1024x768: 2x; 800x600 and
640x480 fill the screen) on black.

Every kernel log line still goes to COM1 and to the disk log
(`C:\SYSTEM\LOGS`, once storage is qualified) and is kept in the console
history. The serial capture is unchanged except for one line,
`[console] quiet boot: log on serial and disk until the desktop starts`;
`L:CPU signature=`, `SELECT_READY`, the menu text, `L:SELECT_SOURCE=menu`,
the `Ciuki VMM F0 build` banner and the supervisor lines are still there.

The console switches on and repaints its history over the splash when any
of these happens: a kernel panic or a double fault (`console_reclaim`),
`[supervisor] desktop unavailable`, `[supervisor] desktop stopped` (the
desktop exited), or no desktop 120 s after the kernel started
(`[console] no desktop 120 s after boot; boot log shown`). From then on it
behaves as in a verbose boot.

## Verbose boots (unchanged console)

The kernel console draws every log line on screen, as before, in these
boots:

| Boot | How it is selected |
| --- | --- |
| Safe mode | `S` in the menu, or `safe=1` in `BOOT.CFG` |
| Serial log | `L` in the menu (sets boot flag bit 11; still toggles bit 1) |
| Probe | `P` in the menu and a selector |
| Test request | QEMU fw_cfg `opt/it.alcybercloud.ciukios/test`, or `probe=` in `BOOT.CFG` (sweeps) |
| Text console | the loader found no usable VBE linear framebuffer |

With a `BOOT.CFG` test request or safe flag the loader also prints its menu
on screen as before. After `S`, `L` or `P` in a normal boot the loader prints
its remaining lines on screen; the menu text itself stays serial-only there.
A loader error is always printed on screen.

## Shutting down

The desktop calls `ciuki_power()` (system call 73): reboot, power off
through ACPI S5 when the firmware tables allow it, or a query of what is
available. Both actions first stop storage cleanly; when power off is not
possible the desktop shows "It is now safe to turn off your computer".
