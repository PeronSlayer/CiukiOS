# Native SuperSavage DOS display provider (f3-32, 2026-10-15)

This implements the provider anticipated by f3-26. Qualification on the T23
remains separate from the host register model.

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
