# Native SuperSavage DOS display provider (f3-32, 2026-10-15)

This implements the provider anticipated by f3-26. Qualification on the T23
remains separate from the host register model.

## fx-biosvm review round 1: firmware owns unqualified panels

Lead amendment 1 supersedes native panel admission and the native-only VBE
full-screen requirement below. The driver reads the active head under its
exclusive mode claims. CR6B ActiveLCD, dual-head/TV and unknown routing cause
a clean refusal before blanking, VRAM transfer or PLL programming. Register
locks and indices are restored and the caller releases all aliases/claims;
this refusal costs no native strike. Single-CRT native mode sequences retain
host coverage; they do not qualify the T23 panel.

Both Win32 present leases and DOS full screen then use the BIOS VM. DOS
uses the existing bounded VBE state-save transaction, 4F02 with LFB enabled,
4F03 readback and 4F01 information. Only 101h/103h/111h/114h are admitted,
with exact guest geometry, pitch and pixel format. A temporary alias borrows
the stopped desktop's same physical aperture and registry generation for the
existing copy and direct-LFB transports. It is revoked before BIOS state
restoration and fbdev commit. A firmware failure remains windowed; it never
retries the unqualified native panel path. Standard VGA modes use BIOS INT10.

Sources, the logged-register replay and APM shutdown coverage are recorded in
[BIOS service isolation, review round 1](../validation/fx-biosvm.md#review-round-1-apm-soft-off-and-panel-mode-ownership).

The fx-biosvm2 state transaction queries the BIOS-defined count of 64-byte
blocks and admits up to 16 KiB. Its ES:BX save/restore area occupies
12000h–15FFFh inside the reserved BIOS workspace, separate from the
10000h scratch/mouse page and 11000h stack. A kernel copy survives other
firmware calls; mode information still uses the scratch tail. The T23's
BX=003Bh (3776 bytes) fits this bound. A refused request retains the guest
BIOS/DPMI continuation, and suspended time does not expire its held-IRQ
guard. Sources and regression boundaries:
[BIOS buffers and continuations](../validation/fx-biosvm2.md).

## war2vbe amendment: guest VBE without a native provider (2026-10-25)

The F3-37 rule below ("a synthetic VBE 2.0 interface only when the native
provider supports the requested mode", "banked VBE remains unavailable") is
superseded for the guest BIOS: the VM's VBE BIOS serves 101h/103h/111h/114h
on every display from the VM's own buffer, in the windowed (banked, 4F05h,
4F0Ah) and linear models, and the window presenter shows them. Native
full-screen scanout still needs this provider; without it full-screen entry
for a VBE mode stays windowed. Decision, sources and tests:
`dos-dpmi-contract.md`, "VBE modes in the DOS window (2026-10-25)".

## T23 c42f556e amendment: VBE full-screen transport (2026-10-25)

Warcraft II's 101h (windowed model) full screen on the T23 ran at a third of
its window speed, froze for 2 s at its first frame and almost met the 5 s
first-frame watchdog. Evidence, research and tests:
[t23-war2-fullscreen](../validation/t23-war2-fullscreen-2026-10-25.md).
Rules added for a VBE full screen, whose scanout is the kernel's copy of the
VM's buffer and palette:

- its trapped VGA ports reach the VGA model as in the window, so the fx-27
  lean exits and DAC burst apply (`dosvm_vga_ports_modeled`); a VGA-mode
  full screen keeps the adapter pass-through;
- the display worker is its transport and never waits for a durable log
  commit while it runs: `first-guest-frame` is a non-blocking
  `display_fullscreen_record` (both transports); entry and return steps are
  unchanged;
- in an 8 bpp mode an effective palette change (DAC or mask differing from
  what the adapter shows) counts as its first frame
  (`source=palette-write`); other display-register writes load nothing and
  earn nothing;
- the fast paths never dereference a borrowed PTE (a linear mode's direct
  LFB lease names provider MMIO outside `P2V`); the ordinary monitor reads
  it through the provider alias;
- each entry starts its own step clock (`display_fullscreen_entry_request`):
  `vm-park step_ms` no longer includes the time spent in the window;
- packed modes program the identity attribute palette (X.Org and Linux
  `vgaHWInit`).

## fx-22 return transaction amendment (2026-10-23)

Trial f441c5bd (Quake quit to the DOS prompt in native full screen, then
Alt+Enter) ends at `vga-capture`; the desktop never came back. On e831e685 the
same capture, 256 KiB through the uncached aperture, took 2230 ms against
226 ms for the identical transfer at entry, while the display worker was
absent from the CPU top list. Evidence, replays and measurements:
[fx-22 validation](../validation/fx-22-fullscreen-return.md).

Windows 9x reference: the VDD saves and restores a full-screen VM's video
state when the display focus changes (RBIL INT 2Fh 4005h/4006h describe the
save/restore callouts), the Windows 3.1 VGA VDD demand-pages video memory and
"must track video memory usage", and a VM that can redraw itself is asked to
repaint instead of having memory saved (OS/2 Museum, Windows 3.x VDDVGA).
VxD services of a VM are not interrupted by the switch; the VM is suspended at
a VMM boundary, and VPICD simulates no interrupt into a suspended VM. Time
critical operations run with Time_Critical_Boost (Win95 DDK,
Adjust_Exec_Priority), above every ordinary time slice.

Expected behaviour, every step bounded, each with a fallback:

| Step | Bound | Fallback |
| --- | --- | --- |
| Return transaction priority | the service task runs entry and return at `SCHED_BOOST_CAP` (15), above the desktop share and the focused-VM preference, below device service; restored afterwards | none needed |
| `guest-revoke` / park | 250 ms; 750 ms while the VM's own synthetic VGA BIOS transfer is in flight, which completes before the guest loses the adapter | revoke, skip capture, VM suspended, `-ETIMEDOUT` (f1-80) |
| Parked VM | blocks on its wake queue (unhold, stop and resume wake it, 20 ms re-check); a suspended VM is not runnable, urgent or the focus preference | — |
| `vga-capture` | registers, then only the plane pages the guest wrote since the last full synchronisation (PTE dirty bits accumulated over the session, cleared after each full BIOS transfer; kernel aperture writes are recorded too), page by page within 750 ms | stop at a page boundary, keep the captured pages, VM suspended, `-ETIMEDOUT`, desktop restored |
| `desktop-restore` | native register restore as before (40 ms retrace waits, 100 us delays) | VBE mode set through the BIOS VM (f3-36), then repaint |
| Step records | each return step waits at most 50 ms (200 ms per return) for its BOOT.LOG commit, then stays queued, so the last durable record names the step that did not finish | queued (f1-80) |
| Stall report | the device-level cpustat task records `fullscreen_stall step=… step_ms=…` once a return step lasts 1 s, every 5 s after | — |
| VM teardown during a switch | `dosvm_destroy` waits up to 3 s for the bounded transaction before any side effect; never from the transaction's own task | `-EBUSY`, VM retained |

Each step record carries `step_ms` (time since the previous step record); one
`[dosvm] fullscreen_return` record per return gives park, capture (pages
captured/wanted and outcome), restore and total milliseconds. A non-owner
kernel copy into the aperture (dosvm_read/write from another task) uses the
model while a VM owns the adapter; the owner's copies mark the pages dirty.

## f3-43 lease and desktop-present amendment (2026-10-18)

The [accelerator ownership amendment](savage-2d.md#f3-43-failed-acceleration-relinquishes-scanout-access)
records the X.Org/Linux source recheck and port/MMIO split. The native
provider can consume an exact, explicitly offered `supersavage-mode` lease
after a failed accelerator has finished CPU I/O and revoked its safe aliases.
Unsafe MMIO and unoffered quarantines still conflict. This changes ownership,
not the f3-36/f3-37/f3-39 mode programming or three-strike policy.

A refusal before framebuffer revocation does not arm `awaiting_frame`.
Rollback after actual revocation still requests a desktop repaint. Native or
firmware restoration already verifies scanout, so its first successful desktop
upload in the current framebuffer generation clears that wait, including a
strip upload. Failed uploads and stale generations do not acknowledge it.
Display Properties mode changes retain their full-frame watchdog requirement.

The desktop reads the existing framebuffer-info query before drawing pending
damage, refreshing its accelerator generation and requesting full damage when
the lease changed. A busy query preserves damage for retry. The notification
counter path also requires a successful framebuffer-info query before consuming
the counter; same-layout returns reuse the desktop surface. RESYNC already
invalidates scanout, and a new generation additionally calls `desk_accel_reset`.
This ensures a first-frame whole-screen upload even if damage precedes the
counter poll, without weakening stale-generation validation in the kernel.

Host coverage checks refused-entry retry inside 100 ms, first upload inside
one frame, unsafe/quarantined lease rejection and a safe accelerator handoff
followed by native entry. These are host-model results; T23 and QEMU evidence
on a built image belongs to the lead.

## f3-37 mode inventory and guest VBE amendment (2026-10-16)

The implementation previously admitted every BIOS mode in `cvga_find_mode`,
not just 03h/13h. Mode X/Y has **no separate BIOS number**: the guest starts
13h and programs its standard VGA CRTC, sequencer and attribute registers.
Keep transferring that actual register file instead of reducing it to a
resolution inferred from the BDA mode byte.

| Guest mode | Raster / depth | Native timing and addressing |
| --- | --- | --- |
| 00h/01h, 02h/03h | 40/80-column text | Existing IBM VGA presets and guest font/character clocks |
| 0Dh, 0Eh | 320/640x200, 16 colours | Existing VGA presets, planar |
| 10h | 640x350, 16 colours | Existing VGA preset, planar |
| 11h/12h | 640x480, 2/16 colours | Existing 25.175 MHz VGA timing, planar |
| 13h and register tweaks | 320x200/240/400/480, 360x200/240/400/480, 640x400 planar tweaks | Guest standard CRTC/25.175 or 28.322 MHz clock, chain-4 or unchained, unchanged plane/page/pan transport |
| VBE 101h / 111h | 640x480, 8 / RGB565 16 bpp | 25.175 MHz; H 640/656/752/800, V 480/490/492/525, negative syncs; pitch 640/1280 |
| VBE 103h / 114h | 800x600, 8 / RGB565 16 bpp | 40 MHz; H 800/840/968/1056, V 600/601/605/628, positive syncs; pitch 800/1600 |
| VBE 110h / 113h (15 bpp), larger modes | Inventoried, not exposed | Outside the four requested linear modes; queries/sets fail |

Register sources: X.Org
[SavageModeInit and SavageCalcClock](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_driver.c)
and Linux
[vgaHWInit/savagefb_decode_var](https://raw.githubusercontent.com/torvalds/linux/master/drivers/video/fbdev/savage/savagefb_driver.c).
CR67 is 01h for 8 bpp and **51h for mobile 16 bpp** (31h would select
15 bpp). CR50 combines width 40h/80h with depth 00h/10h; CR13/51/90/91
encode pitch in eight-byte units. CR3B/3C use X.Org's FIFO midpoint formula;
CR5D/5E hold timing overflow. The PLL integer search uses the existing
180–360 MHz VCO limits with the selected 25.175/40 MHz pixel clock.
Standard CRTC values follow Linux's documented timing equations, with
CR17=EBh from the X.Org packed branch. No engine/MCLK/MIU recipe is added.

The 1024x768 firmware panel keeps SR54–57, SR5B.0, its native timing and
routing; SR58–5A remain read-only diagnostic results. Expansion versus
centering stays the firmware choice. A successful register model does not
qualify panel expansion or visible hardware scanout.

The supplied IBM `Driver/D/A0400/SSAV1014.INF` was checked in full
(DriverVer 01/23/2003, 06.13.10.1290). Contrary to the directive's premise,
it contains **no mode list**: neither 640, 800 nor 1024 appears. Its
`S3SSavage_UserPreferences` removes `PanelCenterExpandMode` and
`SingleController` to reset user preferences; these are not assignments of
expansion values. The Toshiba A0500 INF has the same limitation. Therefore
the table above inventories the source-supported implementation, not an
alleged INF resolution list or a complete hardware capability claim.

The upstream driver is a timing generator, not a fixed BIOS mode table.
`SavageValidMode` rejects rasters larger than the active LCD; `SavageModeInit`
has 8/15/16/24-bit depth branches, with 24- versus 32-bit storage selected
separately. Thus the source-level packed inventory fitting this panel also
includes the following families; only the four modes above are exposed by
F3-37. The conventional VBE numbers below are cross-checked against upstream
[VGABIOS's mode table](https://raw.githubusercontent.com/qemu/vgabios/master/vbetables-gen.c),
not asserted to be an IBM BIOS enumeration.

| Raster | Conventional VBE numbers (8 / 15 / 16 / 24 bpp) | F3-37 disposition |
| --- | --- | --- |
| 320x200 | 13h VGA / 10Dh / 10Eh / 10Fh | VGA 13h only |
| 640x400 | 100h / — / — / — | Standard VGA register tweaks only; packed 100h unavailable |
| 640x480 | 101h / 110h / 111h / 112h | 101h and 111h |
| 800x600 | 103h / 113h / 114h / 115h | 103h and 114h |
| 1024x768 | 105h / 116h / 117h / 118h | Saved desktop only; no guest mode |

There is no portable VBE number for 32-bit storage: the returned mode-info
depth/pitch/masks are authoritative, as on the existing 1024x768x32 desktop.
No additional 15/24/32-bit register recipe or panel stretch coefficient is
programmed. Lower VGA modes use the guest CRTC timing; packed 640x480 and
800x600 use the explicit timings above. The retained firmware 1024x768
panel/desktop timing is restored from its snapshot rather than synthesized.

The [VBE specification](https://pdos.csail.mit.edu/6.828/2018/readings/hardware/vbe3.pdf)
requires 4F00/01 enumeration, 4F02 linear selection and 4F03 current-mode
query, validated complete buffers and 004F/014F status. Implement a synthetic
VBE 2.0 interface only when the native provider supports the requested mode.
There are no guest firmware calls, banked modes, custom CRTC or firmware
entry points. PhysBasePtr is a VM-local token. DPMI 0800 resolves only that
token and a bounded subrange into an allocated private pool block; all host
physical addresses remain refused. This is a narrow amendment of the old
blanket guest-VBE rejection, not general physical mapping.
The DPMI descriptions of
[0800h](https://www.delorie.com/djgpp/doc/dpmi/api/310800.html) explicitly
permit returning an existing region for a virtualized device;
[0801h](https://www.delorie.com/djgpp/doc/dpmi/api/310801.html) releases the
client's mapping reference. Ciuki uses one framebuffer base and accepts
sizes 1..1 MiB at that exact token, not arbitrary token offsets. The private
pool's existing RAM mappings and admission accounting remain authoritative.

Allocate the 1 MiB framebuffer from the VM's already admitted memory pool,
without changing admission limits. Both windowed display and native scanout
consume this backing. Fullscreen copies changed RAM pages under the existing
display transaction, and observes guest PTE dirty bits for first-frame proof.
The provider's temporary BAR1 alias is subordinate to the framebuffer's
quiesced lease and is revoked before restore/fallback. Guest VBE accesses
remain trapped; no extended-register or physical LFB grant is issued.
VBE mode changes while fullscreen request an ordered return before the guest
continues; the selected virtual mode survives that return. Legacy VGA BIOS
and window rendering keep their current path.

## Reference comparison

The owner-supplied `Driver/D/A0400/SSAV1014.INF` identifies
`5333:8c2e/01fc:1014`; `A0500/ssav1179.inf` identifies the Toshiba subsystem.
Both expose `PanelCenterExpandMode`, `SingleController` and active-display
preferences. They supply neither register recipes nor expansion coefficients.
Do not translate those registry names into invented register values.

[X.Org SavageSave/SavageWriteMode/SavageResetStreams](https://github.com/NetBSD/xsrc/blob/trunk/external/mit/xf86-video-savage/dist/src/savage_driver.c)
save extended CRTC, SR08, clocks SR0E/0F/10/11/12/13/15/18/1B/29/30 and
mobile panel SR54–5B. The non-BIOS restore blanks, disables streams, restores
standard and extended timing, pulses SR15's clock-load controls, waits 100 us,
then enables the original stream configuration at retrace. The mobile branch
does not restore the non-mobile MIU registers. Its `SavageSetPanelEnabled`
implementation is actually in
[savage_vbe.c](https://github.com/NetBSD/xsrc/blob/trunk/external/mit/xf86-video-savage/dist/src/savage_vbe.c)
and calls firmware. `savage_vga.c` does not exist at the supplied upstream
path (the local file contains a 404 response). Neither supplies an independent
native panel power sequence. Keep the already active firmware panel powered;
preserve its routing, native timing and expansion registers.

[Streams implementation](https://github.com/NetBSD/xsrc/blob/trunk/external/mit/xf86-video-savage/dist/src/savage_streams.c)
and [definitions](https://github.com/NetBSD/xsrc/blob/trunk/external/mit/xf86-video-savage/dist/src/savage_streams.h)
distinguish secondary-stream enables CR67.2:1 from MMIO primary addressing
CR67.3. VGA takeover clears all three; it must not use the other chips'
CR69.7 sequence. Save both primary streams' addresses, stride and size and
all defined secondary-stream state. Disable both streams before clearing
their primary addresses/strides; restore these before reenabling CR67.
SR54/56 and SR59/5B describe the firmware's existing expansion choice;
preserving them does not promise that a smaller VGA raster fills every panel
pixel. No undocumented stretch algorithm or panel power cycle is introduced.

[Linux savagefb_set_par_int and mode decoder](https://github.com/torvalds/linux/blob/master/drivers/video/fbdev/savage/savagefb_driver.c)
confirm the extended timing and PLL-load sequence, but the panel save branch
tests only Savage/MX. Prefer X.Org's mobile-family branch for SuperSavage.
Do not initialize BCI, reset the engine, change MCLK or touch MIU merely to
display VGA. The display transaction already requires acceleration to stop
with its existing idle proof. Quarantined leases remain an exclusion.

[S3 RE046-B](https://old.vgamuseum.info/images/stories/doc/s3/savage4_registers.pdf)
defines CR31's VGA/enhanced memory controls, CR58 linear access, CR67 colour
selection and CR66's defined-bit mask BBh. These shared controls supplement
the chip-specific upstream sequence. Standard VGA timing/palette/planes come
from the existing IBM/SeaVGABIOS-checked Ciuki model. Mode 13h selects the
standard 25.175 MHz clock; the desktop programmable PLL values are preserved
and reloaded on return. The native packed 640×480×8 recipe uses X.Org's
8-bpp non-BIOS timing/stride setup and an integer search over the same PLL
limits. In the original F3-32 implementation, 101h was only an internal
provider mode. F3-37 above adds the synthetic linear guest interface;
banked VBE remains unavailable.

## Ownership and recovery

Only a single 5333:8c2e display with firmware-enabled I/O/memory decoding,
valid BAR0/BAR1 and a restorable desktop is admitted. After desktop grants
are suspended and fbdev stops acceleration/revokes its mapping, the provider
claims VGA ports, BAR0 and A0000–BFFFF exclusively. A register model uses
the same save/program/restore functions as the kernel. Save original lock,
index and IGA selection state before unlocking; reject dual independent
outputs and inactive/unknown panel states before altering scanout.

Preserve the complete standard VGA state including 256 KiB of planar VRAM
and the desktop's DAC precision. Save extended state before temporarily
selecting the legacy aperture. Restore with the display blank, streams off,
panel/clock state restored and streams reenabled last. Verify writable masks;
read packed stream MMIO as two halfwords. Readback failures retain the saved
snapshot and diagnose the exact register. The f3-36 amendment below supersedes
the original quarantine-only exit policy. Unsupported native states keep
f3-26's refusal.

Checkpoints cover each transaction stage and feed the existing boot log and
reserved-sector black box. Entry requires durable evidence; recovery always
continues despite logging failure. The f3-20 guest VGA register/plane load,
UC aperture grants, task-local ports, first-store watchdog and fault/exit
return remain authoritative. Mode Y uses those same standard VGA ports.
Measure complete return time as well as provider register-restore time;
the 200 ms requirement needs T23 evidence, including persistence and guest
capture costs, and must never abort a necessary recovery.

The display worker polls at 10 ms while full screen or awaiting the returned
desktop frame (100 ms otherwise). The input/stop path records the original
return tick without I/O; `lease_return_ms` measures through native restore
and remapping, and `desktop_frame_ms` includes the first completed compositor
frame. `restore_us` measures the provider itself, including its checkpoints.
None of these counters is proof of visible panel output.

## f3-36 restore correction (2026-10-16)

The T23 a79f840c log records `reg=158 expected=0 actual=8 mask=ff`.
`sw()` encodes sequencer indices as `0x100 + index`: this is **SR58**, not
CR58, CR67 or MM8158. `stage` formerly named the last attempted stage rather
than the stage of the first mismatch. The restored panel/clock stage wrote
SR58 and immediately compared it while the guest VGA timing was still active.

[S3 RE046-B, printed pp. 58–60](https://old.vgamuseum.info/images/stories/doc/s3/savage4_registers.pdf#page=62)
defines SR58 as a read-only horizontal border value; bit 3 is part of that
computed value. SR59 and SR5A are also read-only, and SR5B bits 7:1 are derived
expansion/centering/line-repeat status. Thus 00h -> 08h is not evidence of a
failed writable stream-enable bit. Applying this shared panel layout to the
SuperSavage is supported by X.Org's `HZEXP_BORDER`, `HZEXP_FACTOR_IGA1`,
`VTEXP_BORDER` and `VTEXP_FACTOR_IGA1` definitions in
[savage_regs.h](https://github.com/NetBSD/xsrc/blob/trunk/external/mit/xf86-video-savage/dist/src/savage_regs.h).
The IBM `Driver/D/A0400/SSAV1014.INF` confirms 5333:8c2e/01fc:1014 and panel
preferences, but supplies no register bit definitions.

Continue saving the full panel bytes for diagnostics. Do not write/compare
SR58–5A; compare only SR5B bit 0 and retain checks on writable compensation,
PLL, VGA and stream state. Do not invent a special mask that ignores only
SR58 bit 3. X.Org's eight-byte panel write loop has no readback assertion;
copying it with full-byte equality introduced this failure. Preserve the
existing blank -> streams off -> panel/PLL -> VGA/extended timing -> stream
addresses -> CR67 at retrace -> unblank order, consistent with
[SavageWriteMode](https://github.com/NetBSD/xsrc/blob/trunk/external/mit/xf86-video-savage/dist/src/savage_driver.c),
[SavageStreamsOff/On](https://github.com/NetBSD/xsrc/blob/trunk/external/mit/xf86-video-savage/dist/src/savage_streams.c)
and [savagefb_set_par_int](https://github.com/torvalds/linux/blob/master/drivers/video/fbdev/savage/savagefb_driver.c).

On a native failure, disable further native entries for this boot and retain
the snapshot. Native programming is synchronous CPU I/O, with no DMA, IRQ or
background writer; after guest revocation and native alias revocation it is
safe to quiesce/release those access leases even if scanout verification
failed. This is distinct from proving the mode restored. Registry failures
remain quarantined; no quarantine reset or ownership bypass is permitted.
After release, try the existing mediated `biosvm_video_switch` for the saved
desktop mode. Decode/validate its returned layout before remapping, then
return the desktop grant and require a completed frame. Log native and VBE
results independently; overall success means one verified mode was committed.
If the firmware backend is already disabled, or also fails, report failure
and leave the framebuffer unavailable rather than expose an unverified mode.
Clean exits, Alt+Enter and watchdog `return-or-fault` use this same transaction.

The old 200 ms end-to-end target is contradicted by the T23 evidence:
`restore_us=156307`, but `lease_return_ms=1180`, including guest park/capture,
durable checkpoints and scheduler contention. Retain 200 ms as the provider
register-restore target; revise the native lease/first-frame target to 2000 ms
(1180 ms observed plus 820 ms margin). A VBE recovery gets 4000 ms, allowing
the existing bounded 2000 ms firmware transaction in addition to that native
return allowance. These are diagnostic targets, never reasons to abort
recovery. Report overruns; the independent 10 s missing-frame watchdog remains.
Neither revised target is hardware-qualified by host tests.

## F1-80 return scheduling and logging amendment (2026-10-18)

The return target is 2000 ms through `desktop-restored`, including fallback;
the counter no longer grants an extra 2000 ms after native failure. A needed
hardware recovery must still run to completion rather than stop midway at a
diagnostic deadline. The host replay demonstrates the target for the modeled
slow-aperture and stalled-VM cases; firmware/hardware qualification remains
separate.

Return checkpoints enqueue the existing BOOT.LOG watermark and raw trail for
the storage task. They do not wait for durability or issue device I/O on the
display worker. `EAGAIN` from the watermark request denotes a queued commit;
an unavailable/failed sink produces one `fullscreen_log error` per return.
Entry retains synchronous checkpoints before granting physical access.

The VM park handshake takes at most 250 ms. On timeout, revoke the VGA PTEs
and task-local ports, skip unsafe guest capture, restore the desktop and leave
the VM suspended at its next checkpoint. Do not kill a possible storage lock
owner. A yielded synthetic VGA BIOS continuation cannot use physical transport
after revocation. VGA planes cooperate every 256 bytes, DAC transfers every
16 entries, and retrace polling every 64 reads, retaining indexed-register
pairs and the latch replay as indivisible operations. Research and measured
host evidence: [F1-80 validation](../validation/f1-80-host-replay.md).

## F0-40 recovery wait and strike amendment (2026-10-19)

This supersedes the f3-39 policy for a recovered restore. Native entry
failures still cost one strike per session. A failed native restore costs a
strike only if the display service also fails to commit a verified native/VBE
desktop. Successful VBE recovery costs zero, clears the stale snapshot only
after lease revocation, and leaves native full screen available. Failed or
foreign-generation ownership proof remains a resource refusal.

Both restore retrace waits remain bounded (40 ms / 65536 reads per wait).
Their expiry emits a protected `fullscreen_step=native-restore-retrace-off`
or `native-restore-retrace-on result=-110` and continues to the saved stream
control writes and strict readback. A missing edge in the blanked pipeline
does not itself prove that restoring a register failed. Entry still refuses
a missing edge and reports port 3DAh / expected edge / mask 08h. Delay errors
and stream/VGA/PLL readback errors still fail restore and request fallback;
they identify the first failing wait/register rather than the last stage run.
This adds no register programming or readback-mask relaxation. The prior
`-5 reg=0 mask=0` was ambiguous because a wait/delay error could survive until
the last streams stage; the new records distinguish those cases.

Primary references, reasoning limits, exact host injections and remaining
T23 qualification: [F0-40 reliability audit](../validation/f0-40-reliability-audit.md).
