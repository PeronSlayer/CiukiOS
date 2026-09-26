# T23 desktop freeze and direct setup boot

This records the previous release. The subsequent physical regressions and
current changes are recorded in [runtime recovery](t23-runtime-repair-2026-09-06.md).

The T23 reached the desktop but its contents extended beyond the visible
screen, input stopped responding, and startup emitted beeps. These are physical
reports; QEMU results from the preceding release did not reproduce them.

The old AUTO path selected the card's largest mode when EDID was missing. That
does not prove the LCD can display it without panning. AUTO now requires a
checksum-valid preferred monitor timing; otherwise it uses VGA 640x480x16.
Explicit display presets remain available. After a VBE set, the renderer checks
the actual mode and scanline dimensions, uses the active pitch, and resets the
display start. Mismatched/invalid responses fall back to VGA. BIOS wrappers and
the font query preserve interrupt flags; the desktop enables interrupts before
servicing events. The shared mouse driver and Windows/game launchers are untouched.

The startup player previously ran after drawing the desktop and before the
first input event. It now runs before video/mouse initialization. Quiet startup
does not issue raw IBM EC writes and does not emit fallback beeps. AC97 still
plays the original PCM chime when available. Codec, amplifier and DMA waits
have BIOS tick deadlines plus finite I/O-paced loop budgets for stalled ticks;
the player restores the incoming interrupt masks on exit. This removes known
ways startup could delay input, but does not establish the exact cause of the
physical T23 freeze without another hardware boot.

The graphics checks use VBE functions 03h, 06h and 07h from the
[VESA VBE 3.0 specification](https://read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/vbe3.pdf).
The separate notebook amplifier is documented by the
[Linux ThinkPad ACPI driver](https://www.kernel.org/doc/html/latest/admin-guide/laptops/thinkpad-acpi.html).

## CD menu

GRUB4DOS offers Live CD, Setup, Live CD (safe graphics), and uncompressed
MEMDISK recovery, with six seconds to choose. Setup and safe Live load the same
compressed RAM image and modify only its four-byte SYSTEM/STARTUP.CFG file.
The patched GRLDR and physical-drive remapping are preserved. GRUB's file write
is described in its [upstream manual](https://github.com/chenall/grub4dos/blob/0.4.6a/README_GRUB4DOS.txt).

SHELL consumes SETU/SAFE only on live D: and restores LIVE before launching a
child. Setup opens directly in its existing VGA wizard, without automatic
desktop/audio startup. Cancelling returns to DOS; SETUP reopens the wizard,
EXIT opens a silent VGA desktop. A failed reset blocks automatic setup rather
than cloning a setup request. Installed C: ignores the live-only selection.

## Validation

Evidence is in build/full/t23-boot-fix-2026-09-06. Firmware fixtures are test-only
and are never placed on the release CD. They exercise a stale advertised pitch,
a wrong active mode, corrupt EDID, cleared IF, an invalid active scanline, and
audio firmware that masks timer/keyboard/mouse interrupts. Tests use real
emulated mode sets/framebuffers, actual PS/2 input, and disposable target disks.
The release manifest records completed acceptance and the CD write outcome.

The installer continues to expose Quick and Full formatting; this change only
adds its independent boot path. The direct-setup installation test compares
every installed sector to the source, checks the D: to C: boot patch and GRUB's
disk identity, then boots the target alone. The original Ciuki photo and English
tagline “A modern Retro OS” remain. VS Code retains its 8,000,000,000-byte cap.

Final ISO: `56f6846f50e36613cacb35a8c3104313276381d3692051c33147e72741a0d471` (149356544 bytes).
CD-RW writing to `/dev/sr0` completed successfully (exit 0), with ejection requested.
No separate optical preflight or payload readback was performed.
