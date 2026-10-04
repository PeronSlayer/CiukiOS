# Display settings and native driver information, 2026-10-03

## Research and decision before implementation

The owner requested display settings and advanced monitor/driver configuration
inside Control Panel, alongside removal of visible stutter. Native VirtIO GL
has been validated separately; the unchanged DOS Doom core reaches 34.90 game
tics/render-loop iterations per second, close to its 35 Hz design. A display
settings page cannot make that engine produce intermediate frames.

Primary references:

* [VBE 3.0](https://www.cs.utexas.edu/~dahlin/Classes/UGOS/reading/vbe3.pdf):
  controller/mode information, DDC/EDID, optional CRTC timings and pixel clock.
* [Linux firmware EDID caller](https://android.googlesource.com/kernel/common/+/bce1305c0ece3/arch/x86/boot/video-vesa.c):
  INT 10h 4F15 and a real-mode EDID destination.
* [Linux EDID definitions](https://github.com/torvalds/linux/blob/master/include/drm/drm_edid.h):
  header/checksum, manufacturer/product, dimensions, name descriptor and timing.
* [VirtIO 1.2 GPU protocol](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html):
  GET_DISPLAY_INFO and optional negotiated GET_EDID, bounded control queue replies.
* [QEMU synthetic display EDID](https://github.com/qemu/qemu/blob/v11.1.1/hw/display/virtio-gpu-base.c):
  virtual display metadata is not identification of the physical host monitor.
* [Original Doom loop](https://github.com/id-Software/DOOM/blob/master/linuxdoom-1.10/d_main.c)
  and [Woof uncapped loop](https://github.com/fabiangreffrath/woof/blob/master/src/d_loop.c):
  fixed simulation tics and interpolated render passes are different concerns.

Use a separate DISPLAY.APP, accessible from Control Panel and existing Display
shortcuts. CONTROL.APP already consumes 63,968 bytes of its 64 KiB segment;
SHELL.COM is also close to its bound. Preserve the existing desktop appearance,
English labels and approved icons. Identify the adapter from PCI and active
native-driver state, and identify the display from a validated EDID when exposed.
Show unavailable data honestly. Do not substitute PCI vendor names for monitor
identity or advertise an EDID preferred refresh as the current physical refresh.

Enumerate firmware-supported modes. Apply only modes that the framebuffer and
memory profile can actually support, using a timed preview with explicit Keep
or automatic revert. Refuse disruptive mode changes while DOS sessions own
video. Show current native driver, provider/interface, capabilities and actual
status; unsupported hardware acceleration and custom timing controls must not
pretend to work. Preserve the original DOS engine and its audio compatibility
while evaluating a source-built interpolated engine separately.

## Implementation

`DISPLAY.APP` is registered as static window 18; application-created windows
start at 19. Control Panel's Display icon, the existing desktop Display action,
and Run `DISPLAY` open the same module. Its Screen, Adapter, Monitor and Advanced
tabs separate display identity, PCI adapter, active driver and capabilities.
The driver manager and device manager are reachable from these properties.

`display_probe.c` copies the firmware mode list before further BIOS calls,
validates bounded geometry, pixel storage, pitch and two-page VRAM capacity,
and matches the current VBE framebuffer to a PCI memory BAR. PCI discovery is
bounded to buses 0–7; unmatched adapters are not presented as the active device.
DDC capability is checked before reading EDID. The shared parser verifies the
base-block header, checksum and EDID v1, reports the PNP identity and name, and
decodes a progressive preferred timing with a 32-bit millihertz result. A
preferred timing is not claimed as the current refresh rate. Extensions and
custom CRTC timings are not implemented.

The resident driver negotiates optional VirtIO GPU EDID support, retrieves
display metadata through its existing asynchronous queue and exposes a bounded,
read-only `VM_OP_DISPLAY_INFO` packet to VM 0. The panel's periodic status poll
reads that snapshot; it does not repeatedly probe BIOS or PCI. The packet also
reports open DOS VM records. Apply refuses a mode change while such windows
exist, or when a present session manager cannot return its state.

Exact VBE IDs use a four-byte `Mxxx` profile. VGASETUP validates and previews the
chosen mode for about 12 seconds. Enter commits it through the existing
NEW/BAK transaction; Esc or timeout leaves the existing profile unchanged.
Returning from the command re-enters the desktop and rereads the profile.
Explicit 640×480 modes are accepted; automatic selection retains its existing
minimum desktop preference. Unsupported profiles fall back to automatic mode.
Before positioning, windows are bounded to the available screen dimensions.
The initial 640×480 run exposed unsigned position overflow for DOS's default
680-pixel window, which put that focused window outside the visible desktop;
the dimension clamp fixes the actual cause for native windows. Restoring a
previously maximized window also rechecks saved geometry against the current
screen. Display
metadata arriving after an asynchronous driver rebind now triggers a repaint
on every properties tab, so the monitor name also recovers without pressing F5.

The original DOS Doom executable remains unchanged. A focused review of
`build/external/doom-vanille/d_main.c`, `d_net.c`, `i_ibm.c`, `i_sound.c` and
`scripts/build_doom_vanille_probe.sh` found DMX audio but the same minimum-one-tic
35 Hz render loop. The pinned Doomgeneric/Ciuki bridge is currently silent and
also couples simulation and rendering. Neither existing alternative provides
an audio-compatible interpolated renderer. Removing that motion cadence needs
engine work: separate fixed simulation from presentation, retain previous
camera/actor/weapon state and interpolate render-only frames. Display drivers
cannot supply those missing engine states.

## Validation

The capped full build completed, including the sanitized Windows archive.
After the focused fixes, `DISPLAY.APP` is 17,899 bytes and reserves 28,112
bytes. `SHELL.COM` is 60,864
bytes, within its 60,928-byte bound. The updated GPU protocol fixture passed
82 checks, including optional EDID retrieval; the shared EDID parser passed
valid, malformed, truncated, interlaced and 120/144 Hz cases. All builds and
checks ran sequentially.

The first runtime passed the four-tab display, verified VirtIO PCI `1AF4:1050`
and a valid `QEMU Monitor` EDID, automatically reverted a 640×480×32 preview to
800×600×32, then kept that exact `M142` mode. It exposed the window-size bug
above. That first run is not recorded as an overall pass.

The corrected mode run (`runtime-final`) verified automatic EDID-name refresh
after rebinding, 640×480×32 selection and a visible new Doom game at that size.
The remaining guard check was then performed separately, without repeating
the mode previews. It exposed incorrect event dispatch in the new notice
window: Display Properties was drawing/handling keys inside its own message
box. The final module gives that window its own dialog drawing and input and
blocks parent actions while the notice is open.

`runtime-guard-final/results.json` passed on the shipped module and shell:

* Control Panel → Display detected `QEMU VirtIO GPU`, native VirtIO 2D and
  `QEMU Monitor`.
* A new Doom game was started. PS/2 mouse input selected Control Panel's task
  button and opened Display while the game VM stayed alive.
* Apply reported `[DISPLAY] blocked DOS windows open` and displayed the proper
  notice. Enter dismissed it, Esc closed Properties, and the DOS task button
  returned keyboard focus to the game.
* F10/Y ended Doom normally (`[DOSVM] ended`). Native GPU remained enabled at
  800×600×32, error stage 0, 3,020 commands submitted and completed. Those
  counters are not an FPS measurement.

These are focused component/runtime checks, not a claim that one final boot
repeated every scenario. Input was monitor-injected through PS/2, not a new
physical keyboard test; AC97 used a silent host backend. Captures read KWin's
actual GL scanout. The pointer matcher explicitly converts the RGBA capture to
RGB in memory. All QEMU runs used one KVM CPU, 256 MiB guest RAM, a disk snapshot,
768 MiB host memory cap, zero swap, CPUQuota 200%, TasksMax 128 and a timeout.
QEMU was closed before rebuilding or packaging. Evidence remains under
`build/tests/display-settings-2026-10-03/`.

An intermediate monitor-injected Ctrl+Esc attempt was followed by a DOS-window
close event. The code audit confirmed that the VMM focus-release handler does
not itself request window destruction, but no chronological raw-input trace
was captured to isolate the close. This shortcut remains an open observation;
the final guard check uses normal mouse focus and does not claim to resolve it.

The final image contains the rebuilt Display module and shell; the Windows ZIP
was repackaged afterward. CRC/integrity checks passed, all eight configured
private payload paths are absent, and 28,083 free FAT16 clusters are zeroed.
The updated module, shell and resident GPU module match between local build,
full HDD image and sanitized archive. Windows and physical ATI/NVIDIA hardware
were not runtime-tested.

Final SHA-256:

* Full image: `484bcd7a0c9b1c8f4de17ea2fb66f62a4c7c51c090659142458b91e666111f5a`
* Windows ZIP: `d1a3ff811b2d6da97f4daf06492fce5b1ec6e69fad2bd815cd00405f342e0938`
* DISPLAY.APP: `52666361cffc3050e1e9e923d607094ea10530eb7725df531e47d282aebca4c9`
* SHELL.COM: `8cb352c1ce068a34f564c3cc6c95a6d024a49ae20c8eb098b7b77fa53416dd7d`
* CVSESS.DLL: `7d3ee9208b93807c7cde69e068d5a56da063fa5bbfb37451e5482b0314fe20e9`

The unchanged Doom core remains SHA-256
`799a20c759567cebb530b7d8b1e7765b13734be5af7f97367f6aa81d87b636da`.
Neither these checks nor the prior 34.90 Hz measurement demonstrate absolute
absence of visible stutter. Interpolated game rendering remains unfinished.
