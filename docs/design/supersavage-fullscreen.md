# Native SuperSavage DOS display provider (f3-32, 2026-10-15)

This implements the provider anticipated by f3-26. Qualification on the T23
remains separate from the host register model.

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
limits. Mode 101h is an internal provider mode; this change does not add a
guest VBE API or banked VBE memory transport.

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
