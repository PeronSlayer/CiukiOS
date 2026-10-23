# VIA UniChrome native 2D port

Directive: f2-108. This record describes host-tested code, not physical
qualification. VBE remains the mode provider.

## Primary sources and repository comparison

- Linux **v6.12**, [`drivers/video/fbdev/via/accel.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/via/accel.c),
  `hw_bitblt_1`, `viafb_set_bpp`, and `viafb_wait_engine_idle`, and
  [`accel.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/via/accel.h):
  the four required devices use the original register layout, byte pitches
  divisible by eight, 12-bit coordinates, 16/32 bpp, and independent decrement
  bits for overlapping copies. The upstream idle routine logs expiration;
  Ciuki returns an error and quarantines instead. Unsupported 24 bpp punts.
  The original queue-empty bit must be set as well as all three busy bits
  being clear; the Linux header's misleading `VR_QUEUE_BUSY` name does not
  reverse the polarity of its wait loop. A zero status is never idle proof.
- Linux v6.12 [`via-core.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/via/via-core.c),
  `via_pci_table`, `viafb_get_fb_size_from_pci`, `via_pci_setup_mmio`, with
  [`chip.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/via/chip.h)
  and [`hw.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/via/hw.h):
  BAR0 is framebuffer, BAR1 is registers. Firmware UMA allocation is read from
  the matching northbridge function; unknown encodings are refused instead of
  using Linux's guessed 32 MiB default. No BAR sizing or configuration writes.
- Linux v6.12 [`via_modesetting.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/via/via_modesetting.c),
  primary address, pitch and depth functions, and `hw.c`'s
  `disable_second_display_channel`: read back the existing primary scanout,
  and refuse nonzero origin, pitch/depth disagreement, or enabled IGA2.
  No PLL, panel, timing or output-route programming is imported.
- OpenChrome, NetBSD source mirror **947c2727683984b334ebe249fd981dad3e7a2f44**,
  [`external/mit/xf86-video-openchrome/dist/src/via_ums.c`](https://github.com/NetBSD/xsrc/blob/947c2727683984b334ebe249fd981dad3e7a2f44/external/mit/xf86-video-openchrome/dist/src/via_ums.c),
  `viaDisableVQ`: use the original-engine queue-disable recipe, after idle.
  Its `viaMapMMIO` and `viaMMIOEnable` also establish the access prerequisite:
  save SR10/SR1A, unlock extended registers with SR10 bit 0, and enable the
  primary original-engine MMIO bits (`SR1A | 0x68`). Use VGA MMIO aliases,
  preserve their index latches, check readback, and restore the saved access
  state only after a successful stop/idle proof. No timing bits are changed.
  Linux's VQ allocation/enable and host-data FIFO writes are not imported.
  This port uses synchronous framebuffer-to-framebuffer MMIO commands only;
  optional WRITE stages one bounded row in claimed UMA memory and then copies
  it. It never enables PCI bus mastering, DMA, AGP or GART.
- Linux v6.12 [`tridentfb.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/tridentfb.c)
  and [`include/video/trident.h`](https://github.com/torvalds/linux/blob/v6.12/include/video/trident.h):
  `1023:8400` and `1023:8500` are CyberBlade i7/i1, not `hw_bitblt_1` devices.
  Their sole native owner is f2-113 Trident. This driver matches only
  `1106:3122`, `7205`, `3108`, `3118`; later H5/M1 devices are excluded.
- Linux v6.12 `viafbdev.c:758–904` and `accel.c:471–501`: the cursor callback
  refuses LCD, and its shape upload uses the mask without the image plane.
  That does not establish Ciuki's distinct foreground/background pixels.
  Native cursor hooks therefore refuse cleanly and bit 1 stays clear. Cursor
  packing, LCD routing and signed positions require a separately qualified
  implementation; no inaccurate cursor is advertised.
- Owner's Windows 98 Resource Kit, read-only
  `build/rskt/wrkc28_8.htm`, Graphics Subsystem, and `wrkc26_5.htm`, Setting
  Graphics Compatibility Options: the DIB engine supplies generic drawing,
  and cursor/blit acceleration can be disabled independently. No proprietary
  text or binaries are imported.

The part A registry already checks the exact framebuffer resource lease,
firmware VRAM extent and unique PCI ownership. The driver repeats ownership
validation before mapping, validates MMIO against RAM/kernel/full-VRAM-aperture
aliases, and validates a live generation before each submission. Generic
Device Manager and Control Panel consume the registered display name.

## Completion, failure and diagnostics

Probe is read-only and issues no engine commands. Setup first proves idle,
checks scanout, disables the VQ, and saves engine configuration. An activation
pixel probe runs only through `fbdev_accel_probe`, saving and restoring an
8x8 block under exclusive drawing ownership. It compares native FILL, both
COPY directions, and WRITE (when available) with the software reference.
No failed overlapping copy is replayed by the driver.

Every poll has a monotonic deadline and frozen-clock iteration cap. Presenter
pause hooks are called during completion waits; a cancellation does not count
as idle. Quarantine disables submission permanently for the boot. CPU access
is enabled only after idle and cursor-disable readback; unsafe failures keep
the rejecting ops and all mappings/claims. Stop never restores an enabled
firmware cursor or re-enables a queue. No reset is used as an idle proof.

The attempt file `C:/system/settings/via2d.cfg` records the driver and PCI
identity. A nonempty file, unreadable file, failed commit or close blocks
activation. The pre-existing legacy `accel.cfg` is also checked and is never
cleared by this port. Marker I/O stays outside the presenter. Diagnostic
preflight retains previous markers; the generic DM9 diagnostic does native
commands with software replay excluded and reports their comparison mask.

Optional MONO/INVERT/KEYED/LINE/batch interfaces are absent from this part A
branch and remain with f2-101d. No public ABI or SDK header changes are needed.
Private offscreen scratch is never leased to applications.

## Family result

| Family | Result |
| --- | --- |
| VIA UniChrome / UniChrome Pro (f2-108) | Original-layout `1106:3122/7205/3108/3118`: synchronous native FILL/COPY/SYNC and bounded staged WRITE; VBE modes; software cursor. `1023:8400/8500` exclusively reserved for Trident f2-113. Host evidence only; hardware qualification remains with the lead. |

The referenced census and command-semantics contracts were absent from this
part A branch and were read from the lead checkout. This row is also added to
the available display-driver-model family table; integration should carry it
into the census table without replacing the lead's newer document.
