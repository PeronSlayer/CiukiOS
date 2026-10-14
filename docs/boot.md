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
   and then draws no log line. The splash stays until `/Programs/desktop` takes
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

## Disk boot log (F4-16)

`C:\SYSTEM\LOGS\BOOT.LOG` is capped at 1 MiB. Storage qualification
starts a fresh log for the current boot. Until that gate succeeds, including
in safe/read-only boots and the cold-reopen probe, the previous file is left
unchanged and capture stays in RAM. Loader output and panic/raw output do
not enter the disk sink.

| Byte offsets (inclusive) | Contents |
| --- | --- |
| 0–131071 | Protected prefix: the first 128 KiB of ordinary kernel records, never overwritten during that boot |
| 131072–1048575 | 896 KiB runtime ring: periodic summaries and ordinary output after the prefix fills |

The prefix is protected in RAM before mount too (128 KiB). The runtime
staging queue remains 8 KiB; only its oldest bytes can be dropped under
pressure. Producers do no disk I/O or allocation. Formatted records (at
most 255 bytes) are copied under one bounded IRQ lock. The storage worker
copies in 128-byte slices and writes at most one 4 KiB batch per turn,
flushing pending prefix bytes before runtime bytes.

The file grows as records arrive. Runtime output starts at offset 131072
even if the prefix is shorter; FAT zero-fills the gap. Subsequent ordinary
records backfill that gap without touching runtime output. The ring wraps
by durably truncating **only** to offset 131072 and appending a new cycle;
it never truncates or overwrites the protected prefix. The previous runtime
cycle is discarded at that boundary. A record crossing the prefix byte
boundary can continue at the start of the ring. The ordinary FAT/VFS COW
and flush/error behavior applies; a storage error disables the sink without
recursing into logging. Clean shutdown drains both queues and commits.

USB worker scan-rate and HID latency-window summaries use `klog_periodic`
and bypass the prefix even before mount. Recurring framebuffer performance
summaries (after the first sample) and verbose VM heartbeat diagnostics
also use the runtime ring. Serial and console copies keep their existing
format and ordering. Evidence emitted by `rec_emit` remains ordinary output.

For recovery, read the file from the SSD **before another qualified boot**.
The protected records start at byte zero; runtime records start at byte
131072. Ignore NUL padding between them when reading as text. The physical
evidence importer remains unchanged: it imports bounded `f0.log`
transcriptions and records the qualified disk-log identity, rather than
parsing or reordering this raw file. Its 128 KiB acquisition allowlist is
independent of the 1 MiB BOOT.LOG cap. A raw 1 MiB file is not an `f0.log`
transcription.

### Research and serial decision (2026-10-14)

[TI's TL16C550D register descriptions](https://www.ti.com/lit/ds/symlink/tl16c550d.pdf)
(FCR/IIR and LSR, pages 33–37) establish that FCR is write-only, IIR bits
7:6 reflect FIFO enable, THRE means the transmit FIFO is empty, and TEMT
also includes the shift register.
[Linux's 8250 detection code](https://github.com/torvalds/linux/blob/master/drivers/tty/serial/8250/8250_port.c)
distinguishes scratch-capable 16450 devices from broken original 16550
FIFOs and working 16550A FIFOs. CiukiOS retains its scratch presence probe,
restores the scratch value, and now verifies the FIFO off/on transition and
persistent enable. Broken or unverified FIFOs are disabled; their burst
depth is one byte.

The inspected F4-14 sender already checks THRE and reads IIR after its FCR
write. F4-16 strengthens detection and puts the THRE check inside the shared
burst helper. Every boot, panic and timer sender checks readiness immediately
before writing under the IRQ lock. The 1 ms timer performs at most one
readiness read and one verified FIFO burst; it never polls or stalls audio.
At 38400 8N1 the derived line capacity is 3840 bytes/s (3.84 bytes/ms), so a
16-byte burst takes about 4.17 ms on the wire. The host model separates the
shift register from the holding FIFO and advances bytes only with time;
LSR reads cannot clear the FIFO. It reproduces the ungated chunk-loss
hypothesis and verifies clean guarded records. This does not establish the
cause of the T23 capture; physical acceptance remains with the lead.

[Linux's ring-buffer writer ordering](https://docs.kernel.org/trace/ring-buffer-design.html)
also documents that interrupt writers can preempt ordinary writers. CiukiOS
keeps its UP IRQ serialization for serial records and bounded complete
formatted disk records. The protected prefix is independent of runtime
staging so periodic traffic cannot displace the first boot records.
