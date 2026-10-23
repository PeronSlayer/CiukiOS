# Chips & Technologies HiQV native 2D port

Directive: `f2-122-chips-hiqv.md`. Part A synchronous hooks only; VBE retains
  scanout, panel, clocks and mode ownership. The family and command contracts
were read from the lead checkout because they are absent from this worktree.

## Primary sources and repository comparison

- X.Org `xf86-video-chips`, pinned upstream-history mirror commit
  [`9187d76bd4f8d500fc3403b6d4a7bef55508bf2b`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-chips/tree/9187d76bd4f8d500fc3403b6d4a7bef55508bf2b/src):
  `ct_accel.c` FILL, directional COPY and mono-pattern setup/subsequent
  functions are the engine donor. `ct_BltHiQV.h` supplies register definitions
  and the warning against reading BR04 on older HiQV chips. Replace its reset
  and wait loops with bounded XR20 polling, paced delays and presenter slices.
  `ct_driver.c` supplies PCI IDs, MMIO layout and panel/scratch allocation
  constraints. `ct_cursor.c` supplies cursor enable, position, extended palette
  and base programming. Preserve the original notices in adapted files.
- [65555 datasheet, revision 1.1](https://copleyservices.com/65555_Datasheet.pdf),
  sections 4.6, 14 (XR02/03, XR0A, XR20, XRA0–A7), 17 and appendix E:
  MMIO starts at BAR0 + 4 MiB; the engine supports 16/24 bpp; XR20 supplies
  idle/reset/depth. Cursor memory is 4 KiB aligned, with interleaved AND/XOR
  planes and signed-magnitude positions. XR02 confirms device E5, not C5.
  The published Bitsavers 65550/65555 copies were inaccessible (HTTP 403);
  the original 65555 manual was read at the linked mirror instead.
- Owner's read-only Windows 98 Resource Kit `wrkc24_3.htm`, “Display Driver
  Overview”: drawing omitted by the minidriver goes through the DIB engine.
  Repository `accel.h` already exposes pattern/write/legacy mono cursor;
  `fbdev_accel_probe` provides exclusive saved-pixel activation. No new ABI
  or part C operation is introduced.

## Port decisions

Register `chipshiqv` / “Chips & Technologies HiQV” for vendor 102c and donor
IDs 00e0, 00e4, 00e5 (65550/54/55), class 03, BAR0. The census's 00c0/00c4/
00c5 examples are a transcription error; they are deliberately refused.
65545, 68554 and 690xx are outside this port. Lead acknowledgement of the
PCI correction is recorded as an integration decision in the report.

Probe reads only PCI metadata. Activation rechecks the registry's exact
framebuffer owner/lease, enabled I/O and memory decode, aligned BAR0 and safe
physical extents. Claim VGA ports and a UC MMIO page without sizing BARs or
changing PCI decode. Do not read any BR register. Require packed RGB565 or
RGB888 at 16/24 bpp, bounded pitch/extent and a little-endian linear mapping.
Pitch must fit BR00's 13-bit fields and contain a whole multiple of eight
native pixels, preserving the donor's pattern-origin assumptions.
32 bpp punts to software rather than using the donor's 16 bpp emulation.

FILL, COPY and SYNC are mandatory. With safe free VRAM, claim/map two UC pages
after the entire active framebuffer lease: cursor in the first page, an 8-byte
mono pattern and upload staging in the second. Disable these optional paths
when FR1A reports an embedded panel buffer or the scratch claim is unavailable;
never borrow unknown panel storage. WRITE stages a row in VRAM then submits a
screen blit: it never uses the host data port that can stall the PCI bus waiting
for data. No DMA, bus mastering, AGP/GART, native modes or panel writes.

Before publication, DM9 saves a 16×16 block under `fbdev_accel_probe`, compares
native fill, both overlapping copy directions, optional pattern/write with
`swdib`, and checks cursor registers and packing separately. Fallback is never
used in this comparison. Restore saved pixels only after idle and hidden-cursor
proof. Hardware qualification still belongs to the lead.

Timeout quarantines submissions and hides the cursor; quarantine alone never
authorizes CPU access. A separate bounded recovery observes idle or performs
one donor XR20 reset pulse and then proves idle and cursor disable. Failure
retains a cached rejecting guard and resource leases. No automatic COPY replay,
same-boot reprobe or new panic path. The shared legacy `accel.cfg` marker remains
fail-closed for any nonempty/unreadable record; no legacy record is migrated or
cleared without a successful activation from this driver's own persisted attempt.

Host checks: `nice -n 19 python3 -B tests/host/test_chipshiqv.py` (UBSan,
fake register execution, swdib goldens, lifecycle and target object compilation).
`CIUKI_DISPLAY_ASAN=1` adds ASan with default leak detection, matching the
part A suite's opt-in convention; no sanitizer setting is suppressed.
The finite source closure and downloaded UTF-8 hashes are in the adapter's
`import.json`, installed beside its original notices under `/system/licenses`.
