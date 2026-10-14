# Savage 2D presentation (f1-26)

Status: implemented for host validation; T23 qualification belongs to the lead.
Research date: 2026-10-13. Register constants and behaviour are independently
implemented; no upstream implementation is copied.

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
