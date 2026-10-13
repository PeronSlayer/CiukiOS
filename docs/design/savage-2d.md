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
