# Boot hardware check (f0-26)

Select **H Hardware check** during the loader's three-second menu. This sets
CBI flag bit 13, enables the text console on the existing framebuffer, and
runs the diagnostic task after ordinary drivers initialise. It does not start
the desktop or schedule production acceleration. N/L/P boots are unchanged.
A final compact results list stays pinned on screen for a photograph, even
while background drivers continue logging. PASS establishes
the reported measurements; live input windows with zero events establish only
readiness. Hearing the tone remains an operator observation.

Each item has a 15-second deadline. TSC time, calibrated by the existing PIT
service, measures elapsed time independently of delivered timer ticks. ATA
waits and USB bulk commands inherit the diagnostic task's remaining budget;
other tasks keep their production deadlines. No timer reset, UART panic drain,
controller retry or quarantine reset is used to make a diagnostic pass.

The ordered checks are:

1. Exclusive `C:/SYSTEM/LOGS/HWCHK.TMP`, patterned 512-byte write, commit,
   direct block-device readback bypassing the cache, close and delete.
   A pre-existing scratch file fails without overwriting its contents.
2. A private FAT32 volume in that scratch file. Its logical capacity is 70,000
   sectors; only the 1,116-sector metadata/data prefix needs backing storage.
   The fixture writes 128 published bytes, writes an unpublished 512-byte
   reserved extent, discards its private cache without unmounting, then calls
   the production mount checker. It requires dirty recovery, a writable mount,
   the old 128-byte EOF and unchanged published bytes, then cleans up. The live
   system volume is never unmounted or manually given dirty/corrupt flags.
   Two MiB of temporary cache supplies the checker's byte-per-cluster ownership
   workspace; the preallocation buffer is 571,392 bytes. Both are freed.
3. Wait for the ordinary USB owner to enumerate/mount removable storage, then
   read 1 MiB through its real block interface and report time and a digest.
   Absent media skip; present but quarantined/unmounted media fail. No USB
   recovery bypass, reset or write is performed.
4. Retained HID report layout (protocol, report ID, button count, axis/wheel
   bit positions), then decoded mouse events for five seconds.
5. Five seconds of PS/2 key transitions and EC volume changes, restoring a
   changed mixer master afterwards. Each input window shows its first 96
   events and reports both the total and displayed count plus queue loss.
6. Ten seconds of independent elapsed time versus PIT IRQ count, lost ticks,
   maximum interrupts-off duration and the recorded section/address. Two ticks
   of boundary quantisation are allowed; the existing 250 us critical-section
   budget applies. An uncalibrated independent clock skips rather than passing.
7. A 100-record UART burst through the normal quota-controlled queue. Each
   accepted publication receives a ticket; the persistent record descriptor
   advances the sent watermark only when its final byte enters the UART.
   Refusals return ticket zero and increment the whole-record drop counter.
   The result distinguishes accepted, transmitted, dropped and queued data;
   quota exhaustion can make this check FAIL. The page never drains the UART:
   the log-writer remains the sole normal consumer, with panic takeover only.
   This observes local UART submission, not reception by a remote serial logger.
8. The Savage matrix below, including a display-service switch to the listed
   1024x768x16 mode and restoration/verification of the original mode.
9. Codec diagnostics and the existing integer one-second tone through a
   temporary native audio stream, retaining the configured master level.

Every page line uses the ordinary console/UART/BOOT.LOG path. The optional
`blackbox_capture(const char *, size_t)` RAM-producer hook receives the same
line, including its prefix and newline. Integration with f0-25 links this hook
to its recorder: page lines are captured once through `klog`, while the UART
test's direct records retain explicit capture. The page reports the hook as
linked, or unavailable in configurations that omit it. Raw writes still require
f0-25's validated descriptor and reserved extent; hook linkage alone does not
establish disk durability. See the [integration validation](../validation/f0-25-volume-recovery-and-blackbox.md#integration-with-f0-24-and-f0-26-2026-10-15).

## Serial merge decision (f0-24/f0-26, 2026-10-15)

The [TI PC16550D datasheet](https://media.digikey.com/pdf/Data%20Sheets/Texas%20Instruments%20PDFs/PC16550D.pdf),
section 8.6.3 (printed p. 22), distinguishes THRE (holding FIFO empty) from
TEMT (FIFO and shift register empty). The
[TI TL16C550D datasheet](https://www.ti.com/lit/ds/symlink/tl16c550d.pdf),
Line Status Register (printed p. 37), confirms those semantics and the
16-byte FIFO. Rechecked online against `serial.c`: retain f0-24's FIFO probe,
record-limited refills, readiness reads with IF set, one `serial_tx_byte`
lock per output byte and 75% line-rate quota per 1000 ticks. Add f0-26's
accepted/sent counters to those same descriptors, including drop notices in
both counters so later tickets stay aligned. No bitmap or second consumer is
needed. A sent ticket means final-byte UART submission, not TEMT or remote
receipt. At 38400 baud the 2816-byte data allowance admits 85 of the page's
33-byte expanded records in an otherwise empty quota window; remaining
publications drop whole. Interleaved producers and prior admissions can lower
that count. Console, disk and optional black-box capture still receive every
page record independently of UART acceptance.

## Savage matrix and production setting

Research rechecked online on 2026-10-15, against these local reference files:
`savage_accel.c`, `savage_bci.h`, `savage_driver.h`, `savage_regs.h`,
`savagefb_accel.c`, `savagefb.h`, `savagefb_driver.c` in
`build/downloads/savage/`. The failing T23 record supplied in the directive
contains `008641b6 -> b6b6b6b6`, descriptor `10200409`, CR50 `f1`, MM8136
`0007 -> 0000` and CR45 `00 -> c0`. None is a new hardware result.

- [X.Org Savage acceleration](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_accel.c)
  selects the PM descriptor implementation for SuperSavage. It uses byte
  pitch divided by bytes per pixel, bit 0, block-write disable and a
  chip-dependent bit 3; the PM tiling selector is 1.
- [X.Org descriptor definitions](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_bci.h)
  distinguish linear (0), destination (1), 16-bit (2) and 32-bit (3) tile
  formats. The descriptor union in `savage_driver.h` places stride, bpp and
  controls in the high dword; `savage_regs.h` identifies GBD and BCI controls.
- [Linux savagefb setup](https://raw.githubusercontent.com/torvalds/linux/master/drivers/video/fbdev/savage/savagefb_driver.c)
  uses descriptor bits 0 and 3, block-write disable, MM8134=27/MM8136=07 and
  two E0/E1 register pairs through BCI. Its acceleration source supplies the
  GBD four-word solid-fill packet. Neither initializer requires mix-register
  readback equality.
- [S3 RE046-B register manual](https://old.vgamuseum.info/images/stories/doc/s3/savage4_registers.pdf),
  printed pp. 79, 129 and 142, marks CR45 bits 7:5 reserved (defined mask
  15h), specifies halfword engine reads, and describes MM8136 as foreground
  mix, BCI DA upper half, with defined low control bits. Consequently CR45
  `c0` does not prove a cursor-enable failure. **MM8136 returning zero is
  not explained by a documented write-only register**: the manual describes
  read/write mixes and packed reads, while the SuperSavage evidence differs.
  Treat that as an unresolved chip/readback discrepancy; retain both the
  immediate and post-command samples and decide by complete pixel readback.
  This Savage4 manual alone cannot establish SuperSavage-specific semantics.

The sweep is restricted to H boots and reserves 256 KiB of VRAM disjoint from
scanout. It saves that reservation and touched VGA/engine configuration before
experimentation. Candidate fills at nonzero X/Y test both colour and address
interpretation; a marker at the origin would not discriminate stride/tiling.
Every candidate logs its parameters and readback; first exact match (or none)
is retained for each depth. Mode switching occurs only after proven idle and
successful restoration/release of the previous sweep's leases.

There are 96 configurations per depth. For ID 0..95:

| Field | Encoding |
| --- | --- |
| Descriptor bit 3 | on unless ID bit 0 is set |
| BW_DISABLE | on unless ID bit 1 is set |
| Stride | pixels, or qwords when ID bit 2 is set |
| CR50 depth | `(ID / 8) % 3`: current depth, 00, opposite 16/32 depth |
| Descriptor tile bits | `ID / 24`: linear, destination, 16-bit, 32-bit |

`System\Display\Savage`, integer value `EngineConfig`, selects that ID on
the next production activation, without rebuilding. Missing, malformed,
transaction-pending and out-of-range settings retain ID 0 (the current
configuration). The sweep does not persist its winner or silently downgrade
the desktop depth. The normal durable attempt marker and full marker test
still gate production activation. H can run with a retained previous-boot
marker, preserving it; a current-boot quarantine still refuses entry.

A completion timeout stops the matrix immediately. No subsequent candidate,
reset, mode switch, unsafe VRAM restoration or resource release is allowed.
The page reports failure and retains quarantine/attempt evidence. Exact
restoration is required for PASS; it cannot be promised after hardware stops
responding. A BIOS mode-set failure likewise remains a failure rather than
proof of restoration. Hardware and QEMU qualification belong to the lead.

The FAT32 dirty-bit/ownership decision follows the repository's existing
implementation and [Microsoft FAT specification](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf),
sections on FAT[1], FSInfo and directory EOF. No filesystem recovery policy is
changed by this fixture. USB descriptor/event semantics remain those of the
existing [HID 1.11 specification](https://www.usb.org/sites/default/files/hid1_11.pdf)
parser and USB storage owner; this page only observes them.
