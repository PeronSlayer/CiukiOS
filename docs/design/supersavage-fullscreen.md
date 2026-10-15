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
snapshot, diagnose the exact register and attempt native recovery. Failed
recovery quarantines the claims and never invokes the BIOS or exposes a
guessed framebuffer. Unsupported native states keep f3-26's refusal.

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
