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
   `S for safe mode, L for log, H for Hardware check`, for about 3 s.
3. The loader sets the VBE mode (black screen) and loads `VMM.ELF`.
4. The kernel draws the boot splash, the original Ciuki photograph
   (`misc/CiukiOS_SplashScreen.png`) with the tagline and a progress bar
   under its wordmark, as soon as it maps the framebuffer (in `kmain`,
   before the stack-guard entropy collection, the scheduler and the
   drivers; the display accelerator probe is scheduled only at the end of
   driver activation), and then draws no log line. The bar follows the kernel's init phases. The splash stays
   until `/Programs/desktop` takes the display (its display grant) and its
   first frame paints over it.
5. The desktop.

The splash is a deterministic format conversion done at kernel build time by
`scripts/convert_splash.py` (no Pillow): exact 2:1 nearest-neighbour
reduction to 512x384, RGB565, source and output SHA-256 pinned, embedded in
the kernel (384 KiB of read-only data). The console scales it with nearest
neighbour to the largest 4:3 area of the screen (1024x768: 2x; 800x600 and
640x480 fill the screen) on black.

### Boot splash progress (f0-55)

Under the photograph's `CiukiOS` wordmark the console draws the tagline
`Classic computing, one woof at a time.` in CiukiOS Sans
(`assets/fonts/native/CIUKIOS.CFN`, regular, embedded in the kernel by
`src/kernel/arch/font.asm`) and a thin rounded progress bar: 3/8 of the
photograph's width, 6 rows at 1024x768 (4 to 9 rows at other sizes),
centred on row 728 of 768, with anti-aliased ends. Its colours come from
the photograph: a cream track (`#FFF4E4`, the wordmark's white, at 25 %
over the photo) and a fill that runs from Ciuki's fur tan `#D99A41` to
`#F7D099`. No other text is drawn. Splash areas smaller than 320x240 show
the photograph only.

The bar is indeterminate (a quarter-length segment sweeping the track,
animated by the console worker every 40 ms) until the kernel reports its
first phase. From then on it is determinate and moves only when a phase is
reported, never on a timer.

The first frame already shows state (f0-55b). `kmain` paints it before the
PIT runs, at tick 0, with the segment centred on the track; every later
sweep position keeps at least one step (1/40 of the sweep, 12 columns at
1024x768) inside the track, so no frame shows an empty track. A phase that a
quiet boot reports before the paint is kept, and the first frame shows its
level. Until the scheduler starts (stack-guard entropy collection, 335 ms on
the T23 with the 1.0.0 alpha; PIT calibration, 100 ms) no task exists and
that first frame stays as painted. The sweep is then animated by the console
worker (kernel task `console`, created in `sched_init`): `task_sleep_ms`
wakes it every 40 ms from its own kernel timer (PIT IRQ 0), never from log
traffic, and while the splash is on screen its base level is
`SCHED_FOREGROUND + 2`, above the boot task that runs driver activation and
the desktop spawn (`probes`, `P_INTERACTIVE`, including its I/O completion
boost), so the wake preempts init at init's next blocking wait or
`kpreempt_point` (the kernel does not preempt kernel code). At the handover
or any other end of the bar the worker returns to `P_NORMAL`. The phases (`src/kernel/include/ciuki/splash.h`)
follow the existing records, with shares weighted by the 1.0.0 alpha T23
serial timeline (storage and audio dominate):

| Record | Phase | Bar |
| --- | --- | --- |
| `[init] flag ...` | drivers | 4 % |
| `[init] framebuffer ...` | framebuffer | 6 % |
| `[init] input ...` | input | 8 % |
| `[init] ata ...` | ATA | 12 % |
| `[storage] kind=1` | disk scanned | 25 % |
| `[storage] kind=2` | volume mounted | 40 % |
| `[storage] kind=3` | boot identity | 52 % |
| `[init] storage done` | storage | 60 % |
| `[init] files result=` | files | 66 % |
| `[init] audio init done` | audio | 86 % |
| network and USB started | devices | 90 % |
| `[supervisor] bootstrap: spawning` | supervisor | 92 % |
| desktop image loaded | desktop | 97 % |
| display grant | handover | 100 % |

Progress never goes back; a skipped phase (safe mode has no ATA or volume
records) only makes the next step larger. The display grant, installed by
the supervisor before the desktop runs, completes the bar to 100 % and
logs `[console] splash handover progress=1000 complete=1 phases= updates=
claims= deferred= max_claim_pixels= max_claim_us=`; from then on the
console draws nothing, the splash is never cleared, and the desktop's first
frame replaces it. A wake (panic, desktop failure, watchdog) or a mode
change also ends the bar; phases reported after the end are ignored.

When the serial log is on (`serial=1`, the default `BOOT.CFG`; boot flag
bit 1) each drawn phase level is also recorded once, as
`[console] splash phase=<n> permille=<p>` with `<n>` the phase number of
the table above (0 drivers to 13 handover): at most 14 lines per boot, in
order, the 100 % line just before the handover line. A level drawn for
several phases at once (reported before the first paint, or drawn by the
worker after a refused claim) is recorded once, under the last of them.

Each update redraws only the columns that changed, from the embedded
photograph (no framebuffer reads, no allocation, no disk read), in console
claims of at most 1024 pixels with the claim released before any yield. A
claim refused by a busy presenter is retried by the console worker. The
host test `tests/host/test_splash_progress.py` pins the 1024x768 frames at
0 %, 40 %, 100 % and indeterminate (the tick-0 first frame) and checks the
phase mapping, the claim budget, the handover, a first frame painted after
three phases, the sweep driven by worker turns alone, the worker's class
and the phase records.

Every kernel log line still goes to COM1 and to the disk log
(`C:\SYSTEM\LOGS`, once storage is qualified) and is kept in the console
history. The serial capture is unchanged except for
`[console] quiet boot: log on serial and disk until the desktop starts`,
the splash handover line above and the splash phase records;
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
| Hardware check | `H` in the menu; bounded diagnostic page instead of the desktop ([contract](design/hardware-check.md)) |
| Probe | `P` in the menu and a selector |
| Test request | QEMU fw_cfg `opt/it.alcybercloud.ciukios/test`, or `probe=` in `BOOT.CFG` (sweeps) |
| Text console | the loader found no usable VBE linear framebuffer |

With a `BOOT.CFG` test request or safe flag the loader also prints its menu
on screen as before. After `S`, `L` or `P` in a normal boot the loader prints
its remaining lines on screen; the menu text itself stays serial-only there.
A loader error is always printed on screen.

The exact `BOOT.CFG` token `usbscan=0` (f1-87 diagnostic) keeps the initial
USB enumeration and stops the periodic USB rescans; the loader only accepts
the token and leaves it in the copied boot options for the kernel.

The ThinkPad EC volume poll reads EC register 30h once every 2 s by default
(f2-96): each EC transaction costs the T23 two 2.1 ms SMIs, so the poll is
one transaction per 2 s, and the desktop mixer follows the volume keys within
2 s. The exact `BOOT.CFG` token `ecpoll=0` turns the poll off (the mixer then
follows the keys only when the tray volume popup opens) and wins over
`ecpoll=1`, which selects the 1 Hz diagnosis rate. The loader only accepts
the two tokens and leaves them in the copied boot options.

## Shutting down

The desktop calls `ciuki_power()` (system call 73): reboot, power off
through ACPI S5 when the firmware tables allow it, or a query of what is
available. Both actions first stop storage cleanly; when power off is not
possible the desktop shows "It is now safe to turn off your computer".

## Disk boot log

Current layout (f0-22, f0-23, f0-25, f0-34, f0-36; constants in
`src/kernel/include/ciuki/bootlog.h`, publication contract in
[boot-log-publication](design/boot-log-publication.md)). The F4-16 layout
of 2026-10-14 (one 1 MiB file, 8 KiB staging queue, 4 KiB batches) is
superseded.

`C:\SYSTEM\LOGS` keeps the last three boots within the original 1 MiB
budget: when storage qualification succeeds, `BOOT1.LOG` becomes
`BOOT2.LOG`, `BOOT.LOG` becomes `BOOT1.LOG`, and a fresh `BOOT.LOG` gets a
reserved 320 KiB chain. Until that gate succeeds, including in safe or
read-only boots, the previous files are left unchanged and capture stays in
RAM. Loader output and panic/raw output do not enter the disk sink.

| Byte offsets (inclusive) | Contents |
| --- | --- |
| 0–131071 | Protected prefix: the black-box replay (at most 64 KiB), then the first ordinary kernel records of this boot, never overwritten during that boot |
| 131072–327679 | 192 KiB runtime ring: periodic summaries and ordinary output after the prefix fills |

Producers do no disk I/O or allocation. The prefix is kept in RAM
(128 KiB) until it is written; other records go to a 64 KiB RAM queue of 256
fixed 256-byte slots and are classed as protected (boot and event records,
recurring evidence such as the five-second `vm_perf` rows) or droppable
(verbose diagnostics); under pressure the oldest droppable record is
evicted first, whole, and losses are counted per class. The storage task drains aligned 32 KiB blocks about once a second,
and under pressure up to four blocks or 10 ms per turn; the file length is
published every 5 s and at shutdown. The ring wraps by durably truncating
**only** to offset 131072 and appending a new cycle; it never truncates or
overwrites the protected prefix. After an unclean shutdown the next mount
recovers the unpublished tail of the `BOOT*.LOG` files from their reserved
chains instead of trimming it.

The black box (f0-25) is separate from FAT: a CRC32 descriptor in sector 8
of the partition and a 64 KiB raw ring in sectors 32–159 (reserved by
`scripts/build_image.py`), written directly to the block device. It is
replayed on the serial line before the mount and becomes the immutable
`[blackbox]` head of the new `BOOT.LOG`.

For recovery, read the files from the disk **before another qualified
boot** (for example `mcopy -n -o -i <partition> '::/system/logs/*.LOG' .`).
The protected records start at byte zero; runtime records start at byte
131072. Ignore NUL padding between them when reading as text. The physical
evidence importer imports bounded `f0.log` transcriptions and records the
qualified disk-log identity, rather than parsing or reordering these raw
files; its 128 KiB acquisition allowlist is independent of the log size. A
raw log file is not an `f0.log` transcription.

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
