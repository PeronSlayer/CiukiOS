# Savage 2D presentation (f1-26)

Status: implemented for host validation; T23 qualification belongs to the lead.
Research date: 2026-10-13. Register constants and behaviour are independently
implemented; no upstream implementation is copied.

## f2-92: the display service draws with the engine

Written 2026-10-22. Evidence: T23 image 5a10eef4 (`build/codex/t23-5a10eef4-serial.txt`
lines 210–265): `bci_test result=ok … pixels=16`, `caps=3`, `summary
accelerated=1`, the activation completing after `[supervisor] desktop pid=2`.
Validation and expected records: [f2-92](../validation/f2-92-desktop-2d-engine.md).

Findings. The service already existed (syscall 77: FILL, COPY, PATTERN,
cursor) and the compositor called it, but on the T23 the engine became ready
after the desktop's first frame with the same mode generation. The desktop
learned the new capabilities each frame yet never loaded the hardware cursor,
and window-move and scroll copies require it: no window copy ever ran. Only
whole-row solid fills and one-row duplicate copies could, and nothing recorded
whether they did. The completion wait was also bounded by 256 polls at 5 µs
(about 1.5 ms), less than a large fill or copy needs, and a drawing failure
quarantined silently.

Upstream programming used (cited, nothing invented):

| Operation | Source | Words |
| --- | --- | --- |
| Solid fill | savagefb_fillrect, `savagefb_accel.c:65-90` (local `build/downloads/savage/`) | unchanged `4bcc8000`, colour, XY, WH |
| Screen copy | savagefb_copyarea, `savagefb_accel.c:31-63` | unchanged `4bcc0020`/±XP/YP, src XY, dst XY, WH |
| Host image write | X.Org EXA `SavageUploadToScreen`, [savage_exa.c](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_exa.c) lines 433-561: `RECT\|XP\|YP\|CLIP_LR\|SRC_COLOR`, ROP CC, `CLIP_LR(x,x+w-1)`, XY, WH, rows padded to a dword; destination GBD as savagefb_imageblit (`savagefb_accel.c:115-134`) | `4bcc4040`, `(right<<16)\|left`, XY, WH, data |
| Bit fields | `savage_bci.h:68` CLIP_LR `4000`, `:80` SRC_COLOR `40`, `:130` CLIP_LR word, `:48` BCI_RESET (any window offset feeds the queue) | |
| Queue space | X.Org `WaitQueue4`, `savage_driver.c:501-516`, and savagefb `savage4_waitfifo`, `savagefb_driver.c:266-270`: MM48C60 bits 20:0 = used entries; without a COB X.Org sets `bciThresholdHi = 32` (`savage_driver.c:1941`) | ≤ 32 outstanding |
| Offscreen rows | X.Org addresses offscreen memory with the global descriptor below the screen, capped at 2048 lines (`savage_accel.c:271-272`; scissor 2047, [savage_xaa.c](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_xaa.c) lines 319-333) | rows `[ceil(size/pitch), min(2048, cursor page/pitch))` |

X.Org's default `NoPCIRetry=1` (`savage_driver.c:1215`) and its unthrottled
COB-less writes rely on bus retry; CiukiOS keeps the "no PCI retry" rule and
never has more than 32 entries outstanding: each status read bounds the next
burst. RE046-B gives no other queue rule.

Decisions:

- Kernel service (syscall 77, display grant): FILL, COPY (visible or
  offscreen endpoints), new `CIUKI_ACCEL_WRITE` (host-to-screen, clipped:
  source bounds, optional destination clip, rows outside skipped, columns
  bounded by CLIP_LR) and `CIUKI_ACCEL_OFFSCREEN` (`(end<<16)|first`). Writes
  copy user rows into one staging page outside every lock, then submit one
  BCI command per band.
- Waits: service requests poll in 1 ms slices with the existing 5 µs pacing
  and 20 ms deadline (iteration bound scaled to it). Between slices fbdev
  releases the presenter lock and passes a preemption point, keeping the
  full-screen `drawing` claim (no CPU writer, no console pixels) and
  `accel_inflight` (mode changes return EBUSY). Setup, self-tests and kernel
  callers keep the unsliced bound.
- Failure: any timeout, dead MMIO, queue timeout or fault quarantines as
  before; for a drawing request the records `[accel] savage quarantined …
  phase=draw` and `[accel] summary accelerated=0 … used=…` follow after the
  lock is released and the splash/device record flip. Fills and visible
  writes then complete on the CPU once idle is proven; copies and offscreen
  writes return -EAGAIN/-EINVAL and the desktop repaints from RAM.
- First use of writes/offscreen rows: a hardware self-test on the f1-95 8x2
  block (4x2 image write, then the same image written offscreen and copied
  back), read through the LFB, pixels restored after proven idle. A mismatch
  disables writes and offscreen rows (or offscreen rows only); fills and
  copies are unaffected. Record `[accel] savage write_test result=… phase=…`.
- Offscreen lease `savage-offscreen`: exclusive, never CPU-mapped, released
  with the engine (mode change, quarantine). T23 1024x768x32: rows 768-2047
  (5 MiB).
- Indicator: `2D engine: OK (drawing: software)` from the activation until
  the engine has drawn a presenter's request, then `2D acceleration: YES`;
  `2D engine: NO (<reason>)`. Summary record gains `used=0|1` (a second record
  when it turns 1); the device record's READY reason is `accelerated` only
  after use, `engine_ok_drawing_software` before.
- Desktop: same-generation capability gains rerun the accelerator reset
  (hardware cursor, offscreen rows, one software frame), so window moves use
  engine copies; offscreen caches hold the wallpaper/icon background and the
  two front-most ordinary windows' content, uploaded with WRITE between
  frames (16 rows per idle turn) and copied to the scanout only after an
  exact comparison of each row with the composed RAM frame; solid rows stay
  FILLs. Record `[desktop] accel fills=… copies=… writes=… fallback=…
  cache_rows=… frames=… lfb_bytes=… caps=… vram_rows=…` every 2 s with drawing.

Open: the engine's own speed on the T23 (only CPU bus bytes are modelled),
whether firmware/SMI code touches offscreen VRAM, and the image write's
behaviour on silicon (the write self-test decides at first use).

## f1-95: the first fill after the engine reset

Supersedes the f1-69 interpretation and the f1-88 preamble-depth hypothesis;
evidence, sources and the expected T23 records are in
[the f1-95 validation](../validation/f1-95-savage-bci-32bpp.md). The
production descriptor `10200401` is X.Org's `SavageSetGBD_PM` encoding bit
for bit, and the T23 firmware already had CR50 depth `11`. Every T23
production failure was the single self-test fill, the first colour RECT after
the CR66 reset; in the a395ffa8 sweep only the first fill failed and
configuration 8 (same descriptor as 0) passed. Decision: the self-test first
fills an 8x2 block with the byte-uniform colour 0, then checks a 4x2 marker
fill and a 4x2 GBD BitBLT pixel by pixel, restoring the block after proven
idle. No register programming changes; the quarantine stays the safety net.
The first-fill state is a measured inference, not a documented erratum.

## f3-43: failed acceleration relinquishes scanout access

Research rechecked online on 2026-10-18 against X.Org's
[engine/GBD setup](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_accel.c),
[MMIO enable and idle checks](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_driver.c)
and Linux's
[engine setup](https://raw.githubusercontent.com/torvalds/linux/master/drivers/video/fbdev/savage/savagefb_driver.c).
These sources distinguish CPU VGA access from queued BCI commands. Releasing
the CPU port lease does not prove that an outstanding engine command is idle.

The saved-pixel test uses the BCI FIFO at MM10000, status MM48C60 and the
last framebuffer pixel. It performs no VGA I/O on success. Existing setup
requires ports 3C2/3CC (misc output), 3C3 (VGA enable), 3C4/3C5 (SR08/SR26),
and 3D4/3D5 (CR38/39/40, CR66 reset, CR31/50 depth and CR45/55 cursor state).
Failure teardown uses CR45 and restores CR40. Keep that recipe and its
exclusive port claim; do not add register programming to fix ownership.

Decision: after the final CPU VGA access, quiesce/release the port claim
independently of engine idle. Retain unsafe MMIO and the rejecting framebuffer
guard. Once idle is proven, revoke/release MMIO too. If ordinary release fails,
an explicit idle-proof-backed offer permits only an exact-range claim by
`supersavage-mode`, with a fresh registry generation. Unoffered quarantines,
foreign owners and unsafe engine MMIO remain unavailable.

Production accepts the existing device/decode/aperture/geometry verdicts and
only the established production configurations (SuperSavage 1, Savage4 0).
Other `EngineConfig` overrides remain diagnostic candidates, not production
qualification. In particular, the measured failed SuperSavage configuration 0
cannot trigger another production pixel test. The H sweep remains explicit
diagnostics; it has no persisted per-configuration qualification database.
Commit and close the existing boot-volume `C:/system/settings/accel.cfg`
attempt before claims/register access. A missing marker authorization, failed
commit/close, nonempty retained marker or unqualified configuration refuses
the attempt. Only a fully successful attempt clears the marker.

## f1-69: select the measured SuperSavage configuration

Research rechecked online and against the seven local references on
2026-10-15. This supersedes f1-67's SuperSavage bit-3 experiment. The
read-only `a395ffa8/disk-BOOT2.LOG` records 72 candidates at 32 bpp:
23 exact matches and 49 failures. Configuration 0 returns `b6b6b6b6` for
`008641b6`; configuration 1 returns the exact colour, as do 2, 3 and 8.
Every recorded qword-stride candidate leaves `5a5a5a5a` untouched. There
is no 16-bpp result or mode-switch checkpoint in that capture. Kernel
records continue afterwards; the file cannot identify the stalled operation.

The supplied vendor pack was read at its original location in the main
checkout, without copying proprietary files into this worktree:

- `Driver/D/A0400/SSAV1014.INF`, version 06.13.10.1290 (IBM), selects
  `5333:8c2e`, subsystem `01fc1014`, and identifies IXC SDRAM.
- `Driver/D/A0500/ssav1179.inf`, version 06.13.10.1270 (Toshiba), selects
  the same chip, subsystem `00011179`, also IXC SDRAM.

Neither INF installs tiling, block-write or memory-type registry values.
Both set `UseAGPDMA=1`; that Windows preference does not authorize DMA here.
ASCII/UTF-16LE strings in each accompanying `S3SSAVM.SYS`/`S3SSAV.DLL`
expose `Ses_BCI`, the PM display-driver PDB path, and (IBM only) `BCIOnly`,
but no tiling/block-write/memory-type setting that explains this interaction.

[X.Org's PM initializer](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_accel.c)
clears bit 3 for SuperSavage, uses pixel stride and disables block writes
because the required SGRAM type cannot be determined. Its local
`savage_bci.h` explicitly assigns `BCI_ENABLE_TWISTER=0` to SuperSavage;
`savage_driver.h` and `savage_regs.h` confirm the descriptor and selector
layout. Online retrieval of these three headers failed; they were checked
locally. [Linux's initializer](https://raw.githubusercontent.com/torvalds/linux/master/drivers/video/fbdev/savage/savagefb_driver.c)
sets bit 3 across several chip families. Neither explains why its combination
with BW_DISABLE fails on this T23. Record that interaction as measured,
without inventing a silicon mechanism or enabling block writes on SDRAM.

Decision: default to configuration 1 on SuperSavage, retaining configuration
0 on Savage4. At 1024x768x32 this is descriptor `10200401`, CR50 depth `11`,
linear, pixel stride, BW_DISABLE set. The engine preamble, direct GBD and
both queued E0/E1 pairs use the same selected controls. Keep the complete
descriptor in the driver model. Missing/invalid registry values use the
chip default; explicit `EngineConfig=0..95` still overrides it. The exact
saved-pixel test, durable marker, idle proof and quarantine still gate caps.

The host model encodes the measured bit-3/BW_DISABLE failure independently
of readable depth. It checks default/configuration 1 success, configuration
0 low-byte repetition, complete saved-pixel restoration and failed-attempt
retention. Existing renderer tests exercise fills, overlap copies, patterns,
hardware cursor and present paths with the same production descriptor.
Diagnostic `[accel]` records now report `ops_per_s` over `window_ms` at
one-second intervals, alongside cumulative operations and fault counters.
Operations include drawing and cursor activity; firmware/setup packets do
not count. L enables these periodic diagnostics.

The H page now records its first winner immediately and its original-depth
summary before any firmware call. Engine restoration, mode set and mode
restoration have explicit stage records. A 16-bpp sweep requires an exact
original-depth winner, successful restoration and sufficient remaining item
budget. See [the hardware-check contract](hardware-check.md) for watchdog
and enumeration changes and their evidence boundary. Host success is not
a new T23 qualification: the lead must verify boot `bci_test result=ok`,
nonzero caps, correct cursor/presentation, per-second engine use and a
complete H page on the resulting image.

## f1-67: shared descriptor enable and failure readback

Research rechecked on 2026-10-15 against the seven local reference files in
`build/downloads/savage/` and their primary sources below. This amendment
supersedes f1-65's decision to clear descriptor bit 3 on SuperSavage.

The saved `a4f67ac1/disk-BOOT.LOG` contains acceleration records at lines
127–129: `previous_attempt` skipped this boot before register access. It
does not contain a new marker test. The directive reports a failed marker
from an earlier interleaved serial capture; that capture is not among the
two supplied evidence files. `core/output.c:klog` is unconditional;
the driver's activation summary was verbose-only, and successful attempts
suppressed the buffered register block. Emit both for every attempted
activation, with yields between records outside the pixel transaction.

The sequence comparison is:

| Stage | References and kernel decision |
| --- | --- |
| Mode | The fbdev reference finishes firmware-equivalent mode programming before `SavageSetup2DEngine`; X.Org also warns that early GBD writes can lose stride. CiukiOS starts after the firmware mode commits and never alters primary stream addresses/stride, clocks or CR67. |
| Reset/access | Retain the verified port unlock, IGA1 selection, CR66 reset and bounded idle check before packed registers. CR31=`0c`, CR40=`01`; CR50 selects GBD plus 16/32-bpp depth. |
| BCI | Both initializer branches for SuperSavage permit no overflow or shadow buffer: MM48C18 is masked with `3ff0`, MM48C10=`00700040`, MM48C0C=0, then MM48C18 gains `08`. Keep the existing firmware-buffer rejection and readback checks. |
| GBD | X.Org's PM path uses bit 0 and omits bit 3. The fbdev path uses both bits for SuperSavage too. Test the latter with MM8168=0 and MM816C=`10200409` after BCI setup; send that same value through E0/E1 twice and wait for completion before drawing. The engine-only preamble also uses both bits. |
| Masks/mixes | Keep MM8128/MM812C=`ffffffff`, and halfword MM8134=`27`, MM8136=`07`. Setup is idle; no command is submitted until descriptors are programmed. |
| Layout | GBD stride is pitch divided by bytes per pixel: 1024 for pitch 4096 at 32 bpp, rather than 512 qwords. Linear tile bits stay zero. Block-write disable stays set. |

The fbdev [setup and mode decoder](https://raw.githubusercontent.com/torvalds/linux/master/drivers/video/fbdev/savage/savagefb_driver.c)
provide the shared descriptor controls and BCI order. Contrary to the
directive's description, this source sends GBD through BCI, not direct MMIO.
CiukiOS retains both transports. Its
[header](https://raw.githubusercontent.com/torvalds/linux/master/drivers/video/fbdev/savage/savagefb.h)
defines E0/E1 and the pixel-depth fields. The local `savagefb_accel.c`
confirms the existing GBD solid-fill packet.

X.Org's [initialization and PM descriptor path](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_accel.c)
divide byte pitch by bytes per pixel, disable block writes for uncertain
memory types, and differ on bit 3. Its local `savage_bci.h`, `savage_regs.h`
and `savage_driver.h` confirm the selector values and descriptor layout.
This discrepancy is a hardware experiment, not a proven cause or repair.

[RE046-B](https://old.vgamuseum.info/images/stories/doc/s3/savage4_registers.pdf),
pp. 82, 89 and 148–149, describes CR50 depth, CR66 reset/loading controls,
GBD enable and block-write disable. Preserve firmware CR66 loading controls;
they govern scanout shadow registers, separately from BCI shadow status.
The manual covers Savage4, so the SuperSavage bit-3 decision rests on the
reference comparison and requires silicon evidence.

On failure, retain every write-time readback and the fault status. If the
engine has proven idle, capture current values of all initialized registers
before cursor/MMIO teardown, without logging or yielding inside the pixel
transaction. Report these separately from the initial write-time values.
For an unproven writer, report cached values explicitly and add no diagnostic
bus reads. BCI E0/E1 readback uses the corresponding MMIO registers; it cannot
observe undocumented internal command state. Periodic counters remain
diagnostic-only; initialization, marker results, capabilities and failure
readbacks have no verbosity gate.

The existing RGB565 engine and Display settings already support 16 bpp.
Exercise 1024×768 with pitches 2048/4096 and descriptors `10100409`/`10200409`
in the host model; the Control Panel test also applies, confirms and persists
the 1024×768×16 selection. No automatic depth downgrade or claim of a working
T23 engine follows from the model. Quarantine still forbids another engine
attempt in the same session, including after a depth change. The next T23
attempt needs a recovery boot with the retained failed-attempt marker cleared
through the established procedure, then either a passing marker with nonzero
caps or the new failure block.
## f3-26: refuse an unrecoverable full-screen handover

Research checked on 2026-10-15 against the local `savage_driver.h` and
`savage_accel.c`, and upstream
[`SavageSave`, `SavageWriteMode`, `SavageResetStreams`](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_driver.c).
SuperSavage selects stream addressing with CR67.3; stream enable bits are
CR67.2:1. CR69.7 is used by other Savage branches, so blindly clearing it
is not the mobile sequence. Mobile restoration also saves panel expansion
SR54–SR5B, clocks and extended CRTC state. Zeroing stream MMIO registers
without preserving the desktop is insufficient. The standard VGA aperture
alone does not supply those resources.

The a4f67ac1 disk and serial captures contain no full-screen transaction,
video cost, remap cost or CPU cost intervals. They cannot locate the failed
instruction or establish the remaining windowed bottleneck. The current
code does establish a recovery defect: a BIOS access fault disables and
quarantines the backend, while both full-screen restore and fallback mode
set need that same backend. Instruction progress cannot prove visible video.

Decision: 5333:8c2e full-screen requests return EOPNOTSUPP with
`reason=supersavage-recovery-unavailable` before suspending grants, stopping
acceleration or invoking firmware. This implements the safe refusal outcome;
it does not implement a native SuperSavage mode provider. That provider needs
a separately reviewed, complete panel/clock/stream save and restore contract
that remains usable after a firmware fault. Do not broaden BIOS permissions,
reset quarantine or infer a panel sequence from the desktop 2D initializer.

Full-screen stages on admitted adapters log before each operation. Hardware
checkpoints wait for the existing storage worker's durable watermark; missing
or failed persistence refuses entry. Recovery continues if persistence fails.
Emulated adapters retain serial evidence without storage waits. BIOS register
records bracket each call; logging waits are outside the cumulative 2 s
firmware budget. Port and memory faults include the denied address. VGA
register readback detects failed loads before guest grants. A native 5 s
watchdog requires an aperture write even if guest instructions keep changing;
this establishes a guest store, not proof that a panel displayed a frame.

L in the boot menu now enables diagnostic sampling for that boot. Four
`vm_perf_live*` records per interval enter only the runtime RAM log queue;
they perform no VFS, console or UART operation. The existing detailed dump
remains available after VM destruction. Probe-selected boots retain their
existing serial evidence stream. On T23, compare live guest/monitor/presenter
time, VGA exits/remap/harvest time and delivered timer/sound IRQs against the
existing CPU sampler's audio refill share. A new hardware run is required
before selecting a dominant-cost correction; this patch claims no lag fix.

The readback check uses the standard writable masks documented by
[FreeVGA sequencer registers](https://www.scs.stanford.edu/22wi-cs212/pintos/specs/freevga/vga/seqreg.htm)
and [graphics registers](https://www.scs.stanford.edu/22wi-cs212/pintos/specs/freevga/vga/graphreg.htm).
It preserves register indices and DAC transaction state. Host tests inject
blanking and aperture-selection mismatches, logging failures and a guest
that changes instruction addresses without producing a first frame.

## f1-65: send the global descriptor through BCI

Research rechecked on 2026-10-15. The local reference files are under
`build/downloads/savage/`. In `savage_accel.c`, `SavageSetGBD` selects
`SavageSetGBD_PM` for `S3_SUPERSAVAGE`. That function and the definitions in
[`savage_bci.h`](https://gitlab.freedesktop.org/xorg/driver/xf86-video-savage/-/blob/master/src/savage_bci.h),
[`savage_regs.h`](https://gitlab.freedesktop.org/xorg/driver/xf86-video-savage/-/blob/master/src/savage_regs.h)
and [`savage_driver.h`](https://gitlab.freedesktop.org/xorg/driver/xf86-video-savage/-/blob/master/src/savage_driver.h)
establish the following layout. The `BMPDESC` union overlays the byte offset
and the `HIGH` structure with the two descriptor dwords.

| Field | SuperSavage linear framebuffer | Existing kernel value |
| --- | --- | --- |
| MM8168, GBD low | Starting byte offset | `00000000` |
| MM816C.0 | `S3_BD64`, include the offset dword | 1 |
| MM816C.2 | Endian selector, zero for little endian | 0 |
| MM816C.3 | `BCI_ENABLE_TWISTER` is zero for SuperSavage; Savage4 uses 8 | 0 |
| MM816C low stride field | Pitch divided by bytes per pixel, with aligned low bits available for controls | `0400`, 1024 pixels for pitch 4096 at 32 bpp |
| MM816C.23:16 | Binary bits per pixel | `20` |
| MM816C.25:24 | Zero for linear; PM uses destination tiling value 1 at either tiled depth | 0 |
| MM816C.28 | Disable block write | 1 |

Thus `10200401` agrees with the PM reference. Bit 0 is a descriptor-size
flag, not an extra pixel of stride. The mask `13ff1ffd` keeps bits 28,
25:24, 23:16, 12:2 and 0; it excludes reserved bits. This readback cannot
prove the BCI's colour interpretation. The failure captured for image
`a738204a` also reads CR50 as `f1`, so leaving CR50 at its firmware depth
does not explain this capture.

In [`savage_accel.c`](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_accel.c),
`SavageInitialize2DEngine` sets CR40=01, CR31=0c, plane masks and halfword
mixes. Its SuperSavage branch supports MM48C18=08 without overflow or
shadow buffers. `SavageSetGBD_PM` writes the `10000001` descriptor preamble,
selects GBD stride with CR50.C1 and writes the complete descriptor. Those
engine settings already match the kernel. The PM function also initializes
the primary and secondary bitmap descriptors; the current drawing commands
select only GBD. Its stream and panel setup belongs to mode programming.
It provides no additional engine depth selector to add here. CR50.5:4
remains explicitly set to 01 for 16 bpp or 11 for 32 bpp. Firmware stream
addresses, stride, timing and CR67 remain owned by the mode provider.

The upstream fbdev
[`SavageSetup2DEngine`](https://raw.githubusercontent.com/torvalds/linux/master/drivers/video/fbdev/savage/savagefb_driver.c)
sends GBD low and high through
BCI register indices E0/E1, twice, rather than relying on direct descriptor
writes. It uses a shared bit-3 control value; retain the chip-specific PM
control instead. The reference header defines SET_REGISTER=96000000 with
the register count at bit 16. Decision: after the existing MMIO descriptor
verification, submit two E0/E1 pairs (`960100e0`, offset, `960100e1`, high)
for SuperSavage, then require bounded idle before the saved-pixel test.
This changes the command path that receives the depth, retaining the
descriptor layout and drawing packets. Setup commands enter the same
submitted/unsafe/idle/quarantine protocol as draws and do not increment
the drawing-operation counter. Savage4 retains its existing setup.

The working cause is stale BCI descriptor state despite correct MMIO
readback. The logs do not identify a silicon latch or prove that cause.
The host model therefore treats separate BCI colour depth as a regression
scenario: direct writes can read back correctly while an old 8-bpp state
replicates `008641b6` as `b6b6b6b6`. Queued register writes repair that
scenario, including a discarded first pair. Lost updates and completion
timeouts must still refuse acceleration. The full marker comparison,
restoration after proven idle, durable attempt and quarantine stay required.
The success diagnostic is `bci_test result=ok`.

The lead must run the canonical QEMU regression gates and then qualify the
same image on T23. QEMU does not model this engine. T23 acceptance requires
`result=ok`, exact marker readback, nonzero caps, no quarantine, correct
fill/copy/pattern output and lower measured desktop `present_us` for the
same workload. A host pass establishes the regression scenario only.

## f4-26: GBD packets, cached idle proof and asynchronous setup

Research rechecked on 2026-10-14 against X.Org's
[`savage_accel.c`](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_accel.c),
[`savage_xaa.c`](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_xaa.c)
(`SavageSetupForSolidFill` / `SavageSubsequentSolidFillRect` moved there),
Linux's [`savagefb_accel.c`](https://kernel.googlesource.com/pub/scm/linux/kernel/git/mripard/linux/+/5a838c3b60e3a36ade764cf7751b8f17d7c9c2da/drivers/video/savage/savagefb_accel.c),
[`savagefb_driver.c`](https://github.com/torvalds/linux/blob/master/drivers/video/fbdev/savage/savagefb_driver.c)
and [BCI definitions](https://sources.debian.org/src/linux/6.1.4-1/drivers/gpu/drm/savage/savage_drv.h/).
The named X.Org `savage_bci.h` endpoints were unavailable; Linux's upstream
header explicitly attributes its corresponding definitions to that header.

The ce2e6ce9 T23 evidence proves CR50=F1 and GBD2=10200401 read back,
yet the submitted fill produces 66666666 for 004a8166. It disproves
CR50 programming alone as the fix. X.Org uses PBD_NEW, then descriptor
offset/high, colour, XY, WH: CiukiOS's existing word order agrees. Linux
instead selects DEST_GBD=0 and SRC_SOLID=0, SEND_COLOR=8000, ROP=CC,
then colour, XY, WH (4BCC8000 for positive X/Y). Select that GBD path
for fill, copy (SRC_GBD=20) and mono pattern (PAT_MONO=3); all operate
on the one validated linear framebuffer. This removes dependence on the
primary descriptor and its inline updates. Preserve CR50 and the masked
halfword GBD verification. Do not program primary-stream MM81C0/4/8 or
CR67: no evidence requires changing scanout. X.Org and Linux both support
SuperSavage without COB/shadow (control 08); retain that supported mode.
Require the full 32-bit marker readback, including its zero upper byte.
This is a researched correction to try on silicon, not a claimed T23 pass.

Supersede f4-19's perpetual idle guard: synchronously completed operations
cache CPU safety. A quarantine permits one final bounded idle proof, whose
result is then cached for the session. A proven-idle startup failure installs
no ops in fbdev; software presents/fills/copies/console never poll hardware.
An unproven submitted writer retains a rejecting guard: it blocks CPU writes
and firmware takeover without repeatedly polling or pretending idle. The
durable failure marker and quarantined resource claims remain intact.

Supersede f4-22's whole-init presenter exclusion: pin the mode with an
initialization flag under the presenter mutex, release the mutex, and run
register setup/reset/polls on the existing normal worker while CPU frames
and console lines continue. Firmware change requests return EBUSY while
that flag pins the mapping. Only the saved-pixel test takes presenter and
console exclusion; its logs are emitted afterwards. Snapshot/periodic
logging avoids partially initialized driver counters. Keep marker file I/O
outside presenter locks. Buffer at most 40 paired register records in static
storage, emit them only on failure after exclusion ends, and remove the
verbose per-access pending/completion stream. Reset timing and bounded
polls remain unchanged. Host tests must interleave real production presents,
fills, queries and console access during setup and reject mode changes.
Only T23 measurements can establish the settings_read <1-s target.

## f4-22: engine depth and initialization thread

Research rechecked on 2026-10-14 against [S3 RE046-B](https://old.vgamuseum.info/images/stories/doc/s3/savage4_registers.pdf),
printed pp. 82, 90, 148–149, [X.Org `SavageSetGBD_PM`](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_accel.c)
and [Linux `SavageSetup2DEngine` / `savagefb_decode_var`](https://raw.githubusercontent.com/torvalds/linux/master/drivers/video/fbdev/savage/savagefb_driver.c).
CR50 bits 5:4 select engine pixel length: 00=8, 01=16, 11=32 bpp;
10 is reserved. Linux sets the depth before its engine initializer ORs in
the GBD stride selection. X.Org's PM path also assumes mode setup has
already selected that depth. CiukiOS cannot make that assumption: the T23
445e91c7 capture changes `0010243c` to `66666666` for marker `004a8166`,
consistent with execution using the low colour byte at 8 bpp. This proves
a writer executed; it does not by itself identify every depth dependency.

Supersede f4-19's preservation of CR50 pixel length: replace bits 5:4 from
the active framebuffer bpp, retain the other bits and OR in C1 for GBD
stride selection. Verify depth/selection readback before submitting a command;
normal stop restores the saved firmware CR50. Retain the linear descriptor,
explicit bpp and pixel stride, direct writes and masked halfword readback.
The directive's “GBD1 bpp” is the X.Org `GlobalBD.bd1.HighPart.Bpp` field:
the hardware register is **GBD2/MM816C bits 23:16**; GBD1/MM8168 is only
the byte offset. There is no second bpp field to program at MM8168.

RE046-B calls CR67 bits 7:4 the RAMDAC colour mode, valid only with streams
disabled. Linux sets that field during full mode setup, not its 2D engine
initializer. X.Org PM changes CR67.3 to select stream addresses/stride while
also replacing primary-stream registers. Preserve the firmware CR67 and
primary stream: neither sequence supports changing CR67 for engine depth
alone, and doing so would risk the stable panel. Host tests assert this.

Supersede the device-work continuation for activation: the existing reusable
P_NORMAL `savage-marker` thread performs BEGIN (durable marker), INIT
(registers and pixel test under presenter/console exclusion), then CLEAR or
STATUS (file I/O after releasing the presenter). Eligibility still precedes
thread creation and persistence. Keep live per-register records, including
nonverbose paired records, and yield the normal task between trace records
and reset-delay slices. No Savage initialization is queued to device-work;
its synchronous 38400-baud diagnostics cannot trigger that callback's 1-ms
budget or delay unrelated device-work. No trace buffer or extra thread is
allocated. Marker lifetime, firmware ownership, quarantine and submitted
writer guards are unchanged. This does not shorten serial transmission;
activation time and trace time remain measured, and yields can add scheduling
time. The host serial-cost model verifies thread placement and the retained
records; the lead must measure boot responsiveness and absence of Savage
work-budget records on the T23.

The SuperSavage host model now checks the global descriptor's pixel stride
and offset and uses CR50/GBD depth for colour lanes. It reproduces the
observed low-byte repetition across a command-declared 32-bit destination
when either depth is left at 8 bpp. This is an explicit regression model of
the captured symptom, not proof of silicon precedence between conflicting
descriptors or a complete emulation of wrong-depth addressing. Positive
cases require consistent mode, CR50, GBD and packet descriptors at 16/32 bpp.

## f4-19: descriptor readback and acceleration proof

Research rechecked on 2026-10-14 against [S3 RE046-B](https://old.vgamuseum.info/images/stories/doc/s3/savage4_registers.pdf)
§7 (printed pp. 129, 148–149), [X.Org engine/GBD setup](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_accel.c)
and [MMIO enable](https://raw.githubusercontent.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-savage/master/src/savage_driver.c),
plus [Linux engine/mode setup](https://raw.githubusercontent.com/torvalds/linux/master/drivers/video/fbdev/savage/savagefb_driver.c)
and [definitions](https://raw.githubusercontent.com/torvalds/linux/master/drivers/video/fbdev/savage/savagefb.h).
The X.Org `savage_regs.h`/`savage_bci.h` URLs could not be fetched; a public
SuperSavage register manual remains unavailable.

The T23 fb3147dc log shows GBD2 written `10200401` and read as `00000000`;
CR40 still reads `01` during fault cleanup. The strict idle check passes.
This proves a GBD readback mismatch, not a CR40 failure or that every
0x8xxx register is dead. Even the low word differs, so changing read width
alone is not a demonstrated hardware fix. The manual requires two **16-bit reads** for 32-bit 2D
registers; a dword read has an invalid upper half. Reserved fields have
undefined readback. Replace the dword GBD comparison with halfword reads
and mask `13ff1ffd` (descriptor size, endian, BCI, stride, depth, tile and
block-write controls). Keep packet descriptors separate from GBD controls.

Both enable paths set VGA 3C3.0, MISC.0 and CR40.0. CR53.4 enables old
MMIO, not BAR0's new MMIO; CR58 concerns framebuffer addressing and CR68
memory timing. Do not guess additional enables. Repeat CR40=01 and set
CR31=0c after reset, then plane masks and separate 16-bit mixes. Use the
shared no-COB/no-shadow BCI branch: disable, thresholds `00700040`, shadow
address zero, enable `08`. X.Org selects `SavageSetGBD_PM` for SuperSavage;
retain its linear 64-bit GBD and block-write-disable layout. Linux instead
loads GBD via BCI twice and sets bit 3 too. These drivers are not identical.
Retain direct GBD writes with documented-width readback, the existing
SuperSavage bit-0 descriptor control and Savage4 bit-3 control. Preserve
CR50 pixel length and set only its GBD selection bits, as upstream does.

The directive's panel constraint excludes upstream SR01 blanking, CR65/67/69,
primary-stream addresses/stride and tiled-surface programming. Retain
firmware CR66 enablement, reset pacing, strict idle proof and quarantine.
Emit unconditional paired register records during initialization (indexed
VGA records identify the bank; index-latch writes are transport). After
setup, fill one saved visible pixel with a different packed value, wait
idle, read it back and restore it only after idle proof. Publish acceleration
only on success. A submitted-command timeout retains a private quarantined
ops guard in fbdev so CPU presentation/console cannot race a pending writer.
Failures retain the durable attempt and cached raw status. Host register
models are regression evidence; T23 qualification remains required.

## f1-55: first idle budget and retained raw status

Research rechecked on 2026-10-14 against
[RE046-B, printed p. 231](https://old.vgamuseum.info/images/stories/doc/s3/savage4_registers.pdf),
[X.Org `WaitIdleEmpty4` and `SavageInitStatus`](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_driver.c)
and [Linux `savage4_waitidle` and chip selection](https://codebrowser.dev/linux/linux/drivers/video/fbdev/savage/savagefb_driver.c.html).
The manual assigns MM48C60 bits 16:0 to occupied command entries, 17 to
BCI idle, 18/19 to 3D/2D idle, 20 to motion-compensation idle, 22 to master
idle and 23 to page-flip pending; bit 21 and bits 31:24 are reserved.
X.Org selects the same `e1ffff/e00000` empty-and-idle predicate for Savage4,
ProSavage and SuperSavage. Linux selects its `a00000/a00000` idle-only
predicate for both Savage4 and SuperSavage. Neither source establishes a
special SuperSavage FIFO decoration to discard. The directive's proposed
chip-specific mask difference is therefore unconfirmed. Retain the strict
X.Org predicate for 5333:8c2e and the existing complete Rev-B alternative
`cfffff/4e0000`; never accept Linux's idle-only value or infer a mask from
an incomplete signature. Decorations outside the selected mask are allowed.

This amendment supersedes f1-27's pacing for the first wait after CR66 reset.
That wait reads immediately, then every 1 ms, through the existing 20 ms
budget (at most 21 reads even if the clock stops). Capture the first eight
raw samples and their measured elapsed times. Emit them after polling, along
with the original first-raw record and any selected mask. Defer verbose
MM48C60 transport completion logging too: synchronous COM1 logging would
otherwise consume the deadline before a second read. Later waits retain the
5 us interval, 256-read cap and 20 ms deadline. A dead window, contradictory
layout or failed delay still stops immediately. This changes neither reset
controls nor the idle proof needed before CPU access.

Cache the last raw MM48C60 and include `status`/`status_valid` in activation
and quarantine records, including periodic records. Preserve the fault value
when a quarantined engine is subsequently observed for CPU safety. After a
failed initialization with an observed status, the existing persistence thread
writes and commits `accel=attempt\nstatus=XXXXXXXX\n` outside presenter/device-work
locks. It never clears the attempt on failure. A failed diagnostic update
retains the original nonempty attempt and reports its error; failures before
the first status read retain the original marker without inventing a value.
An interrupted or annotated nonempty attempt still blocks the next boot.

## f1-49: VGA ports, independent engine probe and background persistence

Research rechecked on 2026-10-13 against S3's
[RE046-B](https://vgamuseum.info/images/doc/s3/savage4_registers.pdf)
§1.2 and pp. 76, 78, 83, 148, 227: new MMIO is enabled by default;
CR40.0 enables 2D register I/O, CR53.4 enables **old** MMIO, and CR3A.7
disables PCI read bursts. These descriptions do not establish a combined
CR40/CR53/CR3A enable requirement for the new VGA alias. GBD1 has an
undefined default; MM48C18 has a zero power-on default, with defined BCI,
overflow and status-update controls. A CR66 engine reset is not proof that
all firmware-programmed BCI fields became zero.

[X.Org's SavageEnableMMIO and SavageMapMem](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_driver.c)
use VGA offset `+0x8000` and a 512-KiB BAR0 window for Savage4 and
SuperSavage, with no alternative SuperSavage VGA offset or CR53/CR3A
enable sequence. No public SuperSavage register manual was located.
The T23's failed alias therefore does not justify guessing another address
or changing those firmware controls.

This amendment supersedes the alias prerequisite below. All operational
VGA accesses, including sequencer/pipe selection, reset, cursor and teardown,
use the exclusive legacy port lease. An optional byte-index CR38 alias
probe logs the port/alias values and match result; it never authorizes or
rejects engine access. After both CR66 reset delays and control readbacks,
read/log MM48C18 independently. Reject a dead response or active firmware
overflow/status-update controls before reading MM48C60; then retain the
f1-27 idle, descriptor and BCI readback checks. Reserved bits and upper
buffer counters are not assumed to have reset values.

The kernel settings FAT implementation has no journal to reuse (`fat_write`
replaces file chains). Choose the directive's asynchronous alternative:
check device/aperture/mode eligibility first, then persist the existing
attempt file on one reusable P_NORMAL kernel thread. The device-work
continuation can claim registers only after write, commit and close all
succeed. Clear on the same persistence thread, outside the presenter mutex.
The desktop can present while marker I/O waits, and the shared device-work
queue remains available. Worker/queue/storage failures retain software
presentation; a committed interrupted attempt still blocks the next boot.
This moves the latency off device-work; it does not claim fewer FAT writes
or a measured improvement to T23 activation time.

## Primary references and repository comparison

- S3, [Savage4 Registers, RE046-B, April 1999](https://old.vgamuseum.info/images/stories/doc/s3/savage4_registers.pdf),
  sections 1, 4, 7: MMIO addressing, CR40/45–55/66, bitmap descriptors and
  engine reset. The [Bitsavers copy](https://bitsavers.org/components/s3/Savage4/RE046-B_Savage4_Registers_199904.pdf)
  is also indexed, but could not be fetched in this session.
- X.Org's [savage_driver.c](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_driver.c):
  `SavageMapMem`, `WaitIdleEmpty4`, `SavageInitStatus`.
- X.Org's [savage_accel.c](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_accel.c):
  `SavageInitialize2DEngine`, `SavageSetGBD_PM`.
- X.Org's [savage_xaa.c](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_xaa.c):
  copy, solid and monochrome pattern packet ordering.
- Linux's [savagefb.h](https://raw.githubusercontent.com/torvalds/linux/master/drivers/video/fbdev/savage/savagefb.h)
  and [Savage BCI definitions](https://android.googlesource.com/kernel/msm.git/+/0181b61a988424b5cc44fe09e6968142359c815e/drivers/char/drm/savage_drv.h):
  PCI IDs, MMIO extent and BCI selectors.

The repository uses firmware-selected linear VBE scanout, an exclusive fbdev
lease, synchronous CPU presentation and a userspace compositor. It has no
native graphics backend. Preserve scanout and introduce a grant-authorized
presentation service; the userspace desktop cannot access registers.

## Programming and ownership decisions

Support Savage4 8a22 and SuperSavage 8c22/24/26/2a–2f, vendor 5333,
class 03. Require one display function, enabled firmware memory decoding,
32-bit memory BAR0 (512 KiB registers), BAR1 matching the VBE LFB, linear
16/32-bpp geometry and a pitch divisible by 16 pixels. No BAR sizing,
relocation, PCI activation, bus mastering, DMA, interrupts, COB or shadow
status. Reject a tiled scanout. Claim registers before access; fbdev owns
the visible LFB. Cursor storage has its own exclusive offscreen lease.

CR66.0 must already enable the engine in the firmware mode; changing it
requires blank/retrace. Preserve it and fall back for modes without that
enablement. This avoids modifying display timing merely to acquire 2D.

Enable register access through CR40, plane masks MM8128/812c, mixes MM8134/8136,
BCI MM48c18 without COB/shadow and GBD MM8168/816c (Savage4 enable bit 3;
SuperSavage bit 0). Packets carry a new primary descriptor, avoiding the
upstream GBD stride update problem. Copy ROP is cc, solid source ROP cc,
opaque monochrome pattern ROP f0. X/Y direction handles overlapping blits;
BCI dimensions contain pixel counts, unlike legacy major-axis registers.

Use MM48c60, requiring all engine idle bits e00000 and an empty FIFO
(0001ffff), following X.Org's combined `WaitIdleEmpty4` mask e1ffff. Each packet is shorter than the 32-entry on-chip FIFO and starts
only after idle; no PCI retry assumption. Drain CPU WC writes before a
packet, poll completion before allowing CPU access, with elapsed deadlines
and an iteration bound if time stops. No asynchronous GE writer survives a
successful operation. Console remains nonwaiting and skips pixels while a
presenter owns the display. Mode changes stop acceleration and hide the
cursor before handing registers to firmware, then revalidate geometry.
Normal shutdown quiesces/releases register and cursor leases and revokes both
aliases; the driver reuses reserved virtual windows when the mode commits.

Any fault/timeout permanently quarantines acceleration and retains its
leases. A timeout is not idle proof: CPU access is refused until a subsequent
bounded idle check succeeds. A partially executed overlapping copy cannot be
replayed from its altered source; desktop reconstructs it from its RAM
composite. No automatic reset/reclaim of a quarantined engine. Permanent
hardware lockup cannot safely promise continued LFB writes.

Rectangle acceleration keeps a last completed RAM frame (width*height*4
bytes, at most 7.7 MB at 1600x1200); allocation failure retains normal
presentation. After a screen copy, only pixels differing from that RAM
frame are uploaded. This verifies changes arriving during a move and prevents
stale chrome/client content. f1-25 may supply its completed buffer for this
role at integration instead of keeping a second cache. Scroll candidates are
bounded to 64 pixels vertically and verified against the completed frame.

The cursor is 64x64 monochrome, using a 4-KiB aligned reserved page within
firmware-reported VRAM. CR45 controls visibility; CR46–49 position (Y high
last), CR4a/4b colours, CR4c/4d address in KiB and CR4e/4f clipping. Select
MS mode in CR55 and encode alternating 16-bit AND/XOR words, MSB first.
Unclaimed or insufficient VRAM keeps the software pointer. Cursor appearance
comes from the desktop's existing arrow bitmap. The approved Ciuki artwork
is unchanged.

The existing damage pipeline remains authoritative. Presentation may use
solid runs and verified screen translations for moves/scrolls, but normal
pixels and exposure still come from RAM. f1-25 is a separate worktree;
integration must retain its newer buffers/damage accounting when merging.

## f1-27: reset before status, deferred activation and durable attempts

Research rechecked on 2026-10-13 against RE046-B §1.2, CR66 (p. 89),
MM48C18 (p. 227) and MM48C60 (p. 231), the X.Org references above
(`SavageEnableMMIO`, PreInit's reset, `WaitIdleEmpty4`) and
[Linux savagefb_driver.c](https://raw.githubusercontent.com/torvalds/linux/master/drivers/video/fbdev/savage/savagefb_driver.c)
(`savage4_waitidle`, `savage_init_hw`). The
[X.Org manual](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/man/savage.man)
documents the status-read bus-lock erratum. The independent review's corrected
BAR0 + 0 mapping is retained; BAR0 + 16 MiB applies to older Savage3D/MX/IX,
not the SuperSavage IX/C 8c2e.

This amendment supersedes the early activation and fixed status mask above.
Framebuffer activation performs no engine accesses. After storage, file services
and driver activation finish, a kernel work item takes the framebuffer mutex.
Before claiming/mapping the engine it reads C:/system/settings/accel.cfg.
Any nonempty marker skips acceleration, including on read-only mounts. An absent
or empty marker permits an attempt only after `accel=attempt\n` is written,
committed and closed successfully on a writable settings volume. Success clears
the file by durable truncation. Failures retain the marker and resources, and
never expose failed initialization to the software presenter. Missing storage,
read/write/commit/close failures, safe mode and unavailable work queues make no
engine attempt. Runtime mode reactivation follows the same work/marker path.

Unlock CR38=48h/CR39=A5h, unlock extended sequencer SR08=06h, select IGA1
SR26=40h, force MISC bit 0, then preserve CR66.0 while pulsing bit 1 for
10 ms set / 10 ms clear. Check unlock/pipe/MISC/reset readbacks before status.
Only CR66 is reset: CR3F's broader memory/3D/AGP reset is outside this directive.
Delays use bounded calibrated `udelay`, in <=500-us slices with worker yields;
delay failures quarantine before another status read. Status waits have both
20-ms deadlines and a 256-read cap, with 5 us between busy reads, independent
of PIT progress. No software timeout claims to interrupt a stalled bus cycle.

Log the first raw MM48C60 before selecting a mask. Accept the strict X.Org
empty/idle signature (mask e1ffff, value e00000), or the documented Rev-B
empty/idle signature (mask cfffff, value 4e0000, including PF=0). Both require
an empty 17-bit FIFO; the layout is fixed for the attempt. Linux's a00000-only
signature lacks X.Org's bit 22 and contradicts the manual's PF semantics, so
it does not establish idle. Contradictory layouts quarantine; busy/zero status
may answer late within the cap. Hardware qualification must confirm the raw
value and selected layout; a host model cannot establish the silicon's layout.

After proven idle, read MM48C18, reject firmware shadow/overflow state, clear
BCI/CI low bits and verify disabled state before **reading or writing** packed
2D registers or GBD. Program masks, mix and descriptors while disabled; verify
GBD/CR40 before enabling BCI and verifying its control readback. Shutdown also
verifies BCI disabled before restoring descriptors. Verbose breadcrumbs remain;
each attempted/declined supported activation emits `ready` or
`quarantined reason=...`, with a separate previous-attempt skip record.

## f1-38: bootstrap register access through exclusive VGA ports

Research rechecked on 2026-10-13 against [RE046-B](https://old.vgamuseum.info/images/stories/doc/s3/savage4_registers.pdf),
CR38/CR39 (printed pp. 75–76) and CR40 (p. 78), and the
[X.Org driver](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_driver.c)
(`SavageEnableMMIO`, `SavageDisableMMIO`, PreInit/Save unlock) and
[Linux driver](https://raw.githubusercontent.com/torvalds/linux/master/drivers/video/fbdev/savage/savagefb_driver.c)
(`savage_enable_mmio`). Both drivers program VGA enable at 3c3h, colour
decode at 3c2h/3cch and CR40.0 for Savage4 and later chips, including
SuperSavage. However, [Linux's transport helpers](https://raw.githubusercontent.com/torvalds/linux/master/drivers/video/fbdev/savage/savagefb.h)
map those VGA addresses onto MMIO at +8000h; they do not use legacy ports.
The reviewed X.Org C file uses VGAIN8/VGAOUT8 macros; its transport headers
could not be fetched, so the directive's claim about their physical port
transport was not confirmed. RE046-B accepts CR39's 101xxxxx unlock pattern;
A0h is sufficient, while A5h also permits writes to CR36/37/68/6f. Use
CR38=48h/CR39=A0h throughout. A5h was therefore valid, but broader than
needed; it was not itself evidence of the T23 failure.

The manual describes CR40.0 as engine-register I/O access, not explicitly
as a gate for the MMIO VGA alias. The upstream comparison does not establish
that gate. The f1-38 port-first sequence implements the written experimental
directive, and the host model exercises its alias-gating hypothesis; the
lead must confirm the cause of the T23 readback failure on silicon.
`SavageDisableMMIO` in the reviewed X.Org source sets bit 0 too; it is not
evidence for clearing it. CiukiOS restores its saved original bit as required.

Before any MMIO access, read/write 3c3h with bit 0 set; read 3cch and write
3c2h with bit 0 set; unlock CR38/39 through 3d4h/3d5h and read both back.
Only these port readbacks can report `crtc_locked`. Save CR40 through the
ports, set bit 0 preserving all others, and verify (`mmio_enable_failed`).
Compare MMIO CR38 with the port value (`mmio_alias_dead`). Only then unlock
SR08, select IGA1, verify MISC, pulse CR66.1 preserving CR66.0, and perform
the existing bounded idle/BCI/descriptor sequence. No MM48C60 read precedes
the completed CR66 reset. Stop and every quarantine path restore CR40.0
through the ports after the last alias access, preserving the current other
bits. Verbose reads have a `value=pending` breadcrumb before the bus cycle
and a numeric `value=` completion; writes include the requested value.

The Savage transport admits byte ports only in [3b0h,3e0h). The driver
requires firmware-enabled PCI I/O and memory decoding and claims that range
exclusively as `savage-vga` before register access. `registry_init` does not
reserve VGA ports; `biosvm` previously retained an overlapping `firmware-video`
claim. Firmware preparation now initializes dependencies without claiming
video resources. `fbdev_change_begin` still sets `changing`, waits for the
presenter, and stops acceleration before firmware calls. Successful Savage
stop restores CR40, revokes MMIO aliases, then quiesces/releases its VGA
claim. Firmware claims video resources inside the subsequent serialized
switch and releases them only after requests, callbacks and temporary VGA
mappings are inactive. Completed VBE errors also release; VM/ownership faults
retain quarantined leases. `fbdev_change_end` then schedules the next attempt.
No shared port lease or overlapping access is allowed. Attempt-marker ordering
and durability remain unchanged.

## f1-41: functional unlock and activation timing

Research checked on 2026-10-13 against [RE046-B](https://old.vgamuseum.info/images/stories/doc/s3/savage4_registers.pdf#page=79),
printed pp. 75–76, 78 and 89. CR38 accepts key pattern 01xx10xx;
CR39 accepts 101xxxxx. CR38.5 and CR39.3 are unspecified key bits,
so 68h/A8h satisfy the keys. The reference does not label those bits
as read-only status, fixed-one reserved bits or lock-state indicators.
RE046-B describes Savage4; the T23 SuperSavage observations establish the
actual readback, not an additional documented meaning for those bits.

Keep both port lock readbacks for diagnosis. Unlock success requires
setting CR40.0 through the ports and reading it back set (`crtc_locked`
otherwise), preserving the other fields. An already-set bit is accepted.
Compare the MMIO CR38 alias with the actual port readback, including its
unspecified bits. CR66 verification ignores reserved bits 2 and 6 but
checks all defined controls (mask BBh), including engine enable and both
reset edges. MISC already checks only colour-decode bit 0. SR26 does not
appear in RE046-B; the retrieved [X.Org source](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_accel.c)
uses named IGA selectors but does not define their bit fields. Retain its
full readback check until a primary reference establishes a safe mask.
GBD descriptor and BCI control checks retain their existing semantics.
The two upstream 10-ms reset intervals and the prohibition on MM48C60
before a verified completed reset remain binding.

Timing records use calibrated TSC microseconds, falling back to PIT
milliseconds. Each register breadcrumb carries cumulative `us=`; final
ready/quarantine records carry activation elapsed time, reset and idle-wait
time, and time spent emitting register breadcrumbs. Marker write/clear
records separately time the durable file operation outside the presenter
mutex. Elapsed values include scheduling and synchronous logging, and a
pending read is still emitted before the bus cycle. The driver polls idle
immediately, delaying 5 us only between busy reads; it keeps the elapsed
deadline and frozen-clock read cap. No reset interval is shortened.

The f1-41 capture fails before CR40/reset/status, so neither reset delays
nor idle polling account for its 385566-us work overrun. `klog` writes
synchronously through `core/output.c` and `core/serial.c`; at the default
38400-baud 8N1 setting each serial byte occupies about 260 us. The host
harness accounts for breadcrumb bytes and injects logging/marker costs to
check attribution. This is a timing model; the lead's T23 capture must
measure the actual marker, logging and bus-access costs.

## Display grant wire service

Syscall 77, `CIUKI_SYS_DISPLAY_ACCEL`: EBX is the noninheritable display
grant, ECX points to a copied 308-byte `ciuki_accel_request`. Its 4-byte
aligned fields are size@0, operation@4, framebuffer generation@8, RGB
foreground@12, RGB background@16, `ciuki_rect`@20, pattern bytes@44,
32 cursor mask rows@52 and 32 cursor image rows@180. QUERY=0 returns a
capability mask (1 rectangles, 2 cursor); COPY=1, FILL=2, PATTERN=3,
CURSOR_SET=4, CURSOR_MOVE=5, CURSOR_SHOW=6. SHOW uses colour as a boolean.
Patterns are opaque 8x8, byte/MSB first, anchored at screen (0,0).

Authority is checked before copying the user request. All operations keep
the grant alive, participate in the display-busy counter and clean up on
thread cancellation. Stale generations return EAGAIN; invalid sizes/geometry
return EINVAL without device writes. Copies require both screen rectangles
in bounds; fills/patterns clip. Rectangle operations work in software on
other cards. Unsupported cursor operations return EOPNOTSUPP so the desktop
keeps its software pointer. Releasing the last grant hides a native cursor.
Existing syscall numbers and wire records are unchanged.

## f0-26: bounded configuration sweep

F0-28 supersedes the diagnostic timeout/continuation rule with durable
pre-command records, unsupported-candidate exclusions and one bounded
CR66 recovery before continuing; failed recovery still quarantines. See
[the safety amendment](hardware-check.md#f0-28-safety-amendment-2026-10-15)
and [research](../validation/f0-28-sweep-serial.md). Production activation
and quarantine policy remain unchanged.

[The hardware-check contract](hardware-check.md) records the reference
comparison, interpretation of MM8136/CR45, complete 96-case matrix per depth,
off-screen save/restore, runtime `EngineConfig` registry selection and the
host evidence boundary. H may use a retained earlier-boot attempt marker as
its durable guard, preserving that marker even on a passing diagnostic. It
never lifts a current-boot quarantine. Production activation continues to
require its ordinary attempt-marker gate and exact marker readback.

## f3-32: native full-screen mode provider (2026-10-15)

[The native provider contract](supersavage-fullscreen.md) supersedes f3-26's
unconditional 5333:8c2e refusal for supported single-panel states. It owns
the VGA ports, MMIO and aperture for the complete DOS transaction and uses
native save/program/restore rather than the firmware VM. Unsupported panel,
PCI or resource states still refuse; quarantine is never reset. The 2D
initializer remains separate and does not program scanout. Hardware panel
visibility and the complete 200 ms return budget still require T23 evidence.
