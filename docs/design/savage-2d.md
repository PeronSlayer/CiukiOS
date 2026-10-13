# Savage 2D presentation (f1-26)

Status: implemented for host validation; T23 qualification belongs to the lead.
Research date: 2026-10-13. Register constants and behaviour are independently
implemented; no upstream implementation is copied.

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
