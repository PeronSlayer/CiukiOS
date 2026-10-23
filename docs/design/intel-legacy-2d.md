# Intel legacy native 2D transports (f2-119)

The i810/i815 ring and i740 FIFO are separate backends. VBE retains modes,
scanout, clocks and memory configuration. These ports do not enable PCI bus
mastering, replace valid firmware GTT entries, issue host-data commands or reset a busy engine.

## Sources and repository comparison

- Linux **v6.12**, `drivers/video/fbdev/i810/`:
  [`i810_accel.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/i810/i810_accel.c)
  supplies the six-dword COLOR_BLT/SOURCE_COPY_BLIT packets, independent byte
  X direction and signed Y pitch, IRING programming and INSTDONE `0x7b` proof.
  [`i810.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/i810/i810.h)
  and [`i810_regs.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/i810/i810_regs.h)
  supply field definitions. The latter identifies Intel PRM **298026-001 R**,
  November 1999, revision 1.0. The
  [archived Intel manual](https://downloads.bl4ckb0x.de/download.intel.com/design/chipsets/manuals/29802601.pdf)
  was located, but the full PDF could not be retrieved in this sandbox; register
  decisions use the pinned upstream files, not an assertion of full PRM review.
  [`i810_main.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/i810/i810_main.c)
  supplies physical cursor addressing, palette sequencing, cursor planes and
  pitch readback. Linux allocates/binds AGP memory; Ciuki has no such allocator.
  Its `i810fb_reacquire_gtt` also calls `pci_set_master`: Ciuki instead requires
  firmware to have already enabled bus mastering, and refuses before activation
  if that prerequisite is absent. No PCI command/decode bit is changed.
  [Intel's format table](https://www.intel.com/content/www/us/en/support/articles/000007975/graphics/legacy-graphics.html)
  and the donor exclude 32-bpp acceleration. Initial formats are RGB565/RGB888.
- X.Org i740 commit **1f13c18a17914a6e919e604d1dc5589eb18f0141**, `src/`:
  [`i740_accel.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-i740/blob/1f13c18a17914a6e919e604d1dc5589eb18f0141/src/i740_accel.c),
  [`i740_macros.h`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-i740/blob/1f13c18a17914a6e919e604d1dc5589eb18f0141/src/i740_macros.h),
  [`i740_reg.h`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-i740/blob/1f13c18a17914a6e919e604d1dc5589eb18f0141/src/i740_reg.h),
  [`i740_cursor.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-i740/blob/1f13c18a17914a6e919e604d1dc5589eb18f0141/src/i740_cursor.c)
  and `i740_driver.c` establish the 12-word LP_FIFO packet, 13-bit byte extents,
  separate X/Y direction flags, indexed PIO idle, cursor base below 4 MiB and
  64-bit-interleaved MSB-left planes. MONO remains absent: the donor explicitly
  records lost host mono data and lockup despite its extra wait. Dynamic 32-bpp
  depth is unimplemented. Do not send BLTDATA or infer i810 compatibility.
- X.Org Intel **83b8528773a0b1e6655fa87c9645a0062cee1359**,
  [`src/legacy/i810/i810_reg.h`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-intel/blob/83b8528773a0b1e6655fa87c9645a0062cee1359/src/legacy/i810/i810_reg.h)
  confirms the i810 display-base mask `0x03fffffc`, DAC precision at
  PIXCONF1 bit 7 and the signed-magnitude cursor fields. DPLYBASE is compared
  using this hardware mask, not to an unmasked PCI aperture address.
- The owner's Windows 98 Resource Kit, chapter 24, `wrkc24_3.htm`, Display
  Driver Overview, confirms that the DIB engine handles unaccelerated on-screen
  operations. Ciuki's existing `swdib.c` is the reference and fallback; native
  hooks never perform hidden CPU rendering. Existing part A has only FILL,
  COPY, PATTERN, WRITE and legacy cursor signatures. New semantic commands
  await the part C dispatcher; no ABI numbers are invented by these ports.

## Reservation and restoration contract

Registry nomination proves the single display function owns the complete active
framebuffer lease. Start rechecks nomination and format before resource claims.
Read-only PCI probe performs no register access, resource claim or command.
Port claims protect indexed VGA selector access; selectors are restored after
each read/write. MMIO is UC, and CPU framebuffer writes drain before submission.

For i810/i815, the integration directive supersedes the original firmware-ring
transport. Allocate 17 contiguous kernel-owned pages: 64 KiB ring and a cursor
page. Claim MM_DMA ownership and a bidirectional DMA pin before GPU exposure.
Use the matching host bridge's read-only SMRAM_MISCC to qualify the 32/64 MiB
aperture. Check all 16,384 PTEs in the 64-KiB table, including entries outside
the smaller aperture, for conflicting firmware mappings and physical aliases.
Borrow only the highest 17 invalid entries above the full active framebuffer
and every valid firmware mapping. Preserve even nonzero invalid PTE contents.
An enabled, reserved GTT, linear unique reserved framebuffer backing, idle
disabled rings and firmware-enabled bus mastering remain prerequisites.

No firmware ring bytes or stolen cursor backing are modified. Save disabled
ring registers and program the owned ring only under the fbdev probe claim.
CPU access uses the direct RAM mapping; WBINVD and the existing WC write drain
publish owned data and GTT changes. These cache operations follow the Gen1
Linux i810/AGP path; the different Gen2 HIC protocol is not substituted.

Stop proves idle, disables/readbacks the owned ring, restores/readbacks the
original ring and cursor registers, restores/readbacks every borrowed PTE,
flushes publication, then unpins and frees the pages. A failed proof retains
the allocation, pin, leases and rejecting guard. A full/ambiguous GTT, missing
matching bridge/aperture, allocation/pin failure or ownership change refuses
activation. The i810 cursor uses the owned page only when firmware has a
configured disabled cursor; no cursor storage means software cursor alone.

The independent i740 cursor still uses a bounded page-aligned off-screen local
VRAM reservation and restores its contents/registers. Both cursors hide before
replacement, translate opacity/image masks and validate signed positions.

The primary-source comparison and integration evidence are recorded in
[integ-video.md](../validation/integ-video.md).

i740 uses local framebuffer offsets and the MMIO LP_FIFO with indexed PIO busy
readback. It requires empty LP/HP FIFOs and idle BitBLT before activation.
Firmware scanout start, pitch and pixel depth must agree with VBE. It neither
allocates a ring nor touches an AGP/GART. With a reserved cursor page, PATTERN
uses eight mono rows at page offset 1024, disjoint from the cursor image and
verified against swdib during activation. WRITE and new semantic hooks remain
absent. MONO host expansion remains disabled even when PATTERN is usable.

All polls have a 20 ms deadline, a frozen-clock cap and presenter pause support.
Timeout/dead reads quarantine without retry or reset. Idle proof is separate
from quarantine: failed proof retains claims, mappings and a rejecting ops guard.
Successful idle plus cursor disable permits software use. Successful stop
restores saved memory/registers/palette and revokes mappings/claims before
returning, so an immediate firmware mode change cannot race deferred cleanup.
Failed overlapping COPY is never replayed by the driver. No new panic path exists.

Before publication, `fbdev_accel_probe` serializes native FILL and both COPY
overlap directions against `swdib` on a saved 16x16 block. Cursor enable/base/
position readbacks are checked separately with a transparent shape. Restore
saved pixels only after idle. DM9's later generic hardware check remains usable.

Attempt files are `C:/system/settings/accel-i810.cfg` and `accel-i740.cfg`.
An unreadable/nonempty own or legacy `accel.cfg` blocks activation. Only a
successfully verified own attempt is cleared; VFS commit and close failures
fail closed, and all file operations stay outside the presenter lock.

## Evidence boundary

Host fake-register traces and packet execution models verify software packing,
refusal, restoration and quarantine. They do not qualify physical silicon,
firmware reservation availability or bus behavior. The lead reviews this
transport and performs physical DM9 checks; there is no matching QEMU device.
