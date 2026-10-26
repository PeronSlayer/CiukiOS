# Qualified default pixel formats (f7-12)

Research and implementation contract, 2026-10-26. This extends the f7-03 and
f7-11 contracts; their ownership, bounded execution, restoration, guards and
sample-space rules continue to apply.

## Primary evidence

* [Microsoft ChoosePixelFormat](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-choosepixelformat)
  specifies requested draw/support flags, exact double/stereo matching unless
  DONTCARE, and closest available precision rather than guaranteed precision.
* [Microsoft PIXELFORMATDESCRIPTOR](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-pixelformatdescriptor)
  distinguishes ICD (neither generic flag), accelerated generic (both), and
  software generic (GENERIC_FORMAT only). Generic flags are description flags,
  not input requirements to ChoosePixelFormat.
* [OpenGL Windows FAQ, 5.030–5.040 and 5.100](https://www.opengl.org/archives/resources/faq/technical/mswindows.htm)
  explicitly gives available hardware priority in ChoosePixelFormat; explicit
  software selection enumerates DescribePixelFormat instead. Microsoft's page
  does not specify a complete numerical ranking; Ciuki retains its existing
  precision ordering within each implementation class.
* [id Software win_glimp.c](https://github.com/id-Software/Quake-III-Arena/blob/master/code/win32/win_glimp.c),
  GLW_InitDriver and GLW_ChoosePFD: automatic depth is 24 above a 16-bit
  desktop; stencil requires D24. Its own enumeration also examines generic
  flags, so DescribePixelFormat must describe the same acceleration class.
* [Mesa Savage visual setup](https://chromium.googlesource.com/chromium/deps/mesa/+/3a0db227ffe90888ad760c61a63226988c974e00/src/mesa/drivers/dri/savage/savage_xmesa.c),
  savageCreateContext/CreateBuffer/InitScreen, exposes D16 in 16-bit colour
  and D24/S8 in 32-bit colour. D16 stencil is software and marked slow.
* [Mesa Savage state](https://chromium.googlesource.com/chromium/deps/mesa/+/6da96107e2467063c72e1ec5804f0618a6ce83d3/src/mesa/drivers/dri/savage/savagestate.c)
  and [register definitions](https://chromium.googlesource.com/chromium/deps/mesa/+/3a0db227ffe90888ad760c61a63226988c974e00/src/mesa/drivers/dri/savage/savage_3d_reg.h):
  zDepthSelect selects D24/S8 on Savage4 class, including SuperSavage;
  zBufWidthInTiles uses 32 pixels for four-byte depth, 64 for two-byte depth.
  Stencil compares reference against stored stencil, and requires Z enabled
  even with the depth test disabled. Float depth stays disabled.
* [Mesa span conversion](https://chromium.googlesource.com/chromium/deps/mesa/+/3a0db227ffe90888ad760c61a63226988c974e00/src/mesa/drivers/dri/savage/savagespan.c):
  hardware words have reversed integer depth in bits 23:0, stencil in 31:24.
* [X.Org Savage DRI 2.3.9](https://www.x.org/releases/individual/driver/xf86-video-savage-2.3.9.tar.bz2),
  savage_dri.c SAVAGEDRIInitVisualConfigs: D24/S8 on 32-bit colour, software
  stencil on 16-bit colour. Checked against the local upstream source under
  build/downloads/f7-sources in the lead checkout, together with Mesa 7.11.2.

The supplied driver pack was inspected read-only. Driver/D/A0400/SSAV1014.INF
and A0500/ssav1179.inf identify SuperSavage ICD ssicdnt.dll. Both are NT-family
packages (IBM version 06.13.10.1290), including the UIU/A1 and UIU/A2
INF copies found by a package-wide search; their CHICAGO INF signature is not proof
of Windows 9x support. No Savage4/9x pixel-format enumeration was established
from that pack. Do not claim its proprietary format indices or infer absent
chip features from missing package evidence. Mesa/X.Org supply the register
and visual evidence for implementation; no proprietary driver is imported.

## Decisions

Stable format indices remain 1–10. Qualification queries are read-only and
use the current surface size and display generation. On qualified modes,
DescribePixelFormat marks software-only attachments generic and describes
qualified attachments as ICD formats. ChoosePixelFormat first matches the
existing flag/buffering rules, then acceleration class, then its unchanged
colour/depth/stencil precision cost. Unqualified modes retain their exact
prior selection and descriptor behavior. Explicit indices remain usable.

D24/S8 uses a new typed native resource: canonical increasing depth in low
24 bits and stencil in high eight bits. Depth values in vertices and clears
use the precision of the selected attachment, with no truncation to D16.
Stencil state is copied, range checked and versioned with the wire records.
The scalar renderer remains an independent recovery/differential reference.
Partial clears preserve the other component and masked stencil bits.

D24/S8 is advertised only after a separate current-mode boot probe compares
coverage/copy exactly, colour within two levels, depth within one D24 unit,
and stencil exactly, with the existing restoration and fault containment.
A 63-by-59 patch includes steep D24 planes and sub-D16 differences, so the
probe detects insufficient barycentric precision rather than testing only
constant or binary-fraction planes.
Failure leaves D16 available only when ownership and engine integrity remain
proved. Allocation failures and unsupported formats return errors, never panic.

Host model evidence does not qualify T23 silicon or establish frame rate.
The lead owns canonical-image and hardware validation.

RGB565/D16 uses typed colour and linear-copy resources with canonical ARGB8888
shadows and physical two-byte destinations. Creation and upload normalize
those shadows immediately, including opaque alpha, so reads before GPU work
observe the same format. Conversion is deterministic RGB
bit replication; destination alpha is absent. Its own current-mode probe is
required. RGB565/D24 combinations remain software because that combination
has no independent gate. The provider still presents canonical 32-bit pixels
through the existing lease and preserves the native GPU copy.

The texture pool is calculated from the same inventory-owned heap, reserving
D24 scratch when it fits with at least the existing 480 KiB fixture pool.
At 1024x768, two XRGB colour buffers plus packed D24 require 9 MiB before
textures and guards; the approximately 8 MiB test heap cannot admit them.
D16 remains the accelerated default there. At 640x480 and 800x600 the separate
D24/S8 gate fits. No framebuffer precision is silently reduced after selection.
Quake's own GLW_InitDriver automatically retries with stencil zero when its
first strict stencil enumeration fails; this is distinct from the less strict
ChoosePixelFormat closest-match behavior. Both paths need host evidence.

The opt-in census uses the extended reference profile and appends a stencil
class tuple: enabled, compare function, reference, read mask, write mask,
fail, depth-fail and depth-pass operations. Values are GL enums except the
byte masks/reference. Its reference eligibility remains independent of the
installed device; neither the census nor choosing a format grants hardware
admission. Actual qualification and generation still govern the provider.

## Integration with the background qualification (f7-12 + f7-13 review)

Checked 2026-10-26 against the f7-13 contract
([validation note](../validation/f7-13-probe-boot.md)) and the review of
both branches. These rules supersede the partial-clear and gate wording above
where they differ; the formats, encodings and tolerances are unchanged.

* **Partial D24/S8 clears are bounded.** A clear that preserves the depth
  component, the stencil byte or unmasked stencil bits is a CPU
  read-modify-write through the tiled alias. It runs after the job's uploads
  and before any engine command of the job, on engines proved idle at entry,
  in windows of at most 128 pixels. Each window borrows only AGD/tile and
  restores both before the presenter mutex is released. An ordinary job keeps
  its drawing claim and `accel_inflight` across the windows and yields only
  the presenter mutex and its preemption hold (`fbdev_render3d_yield`, the
  f2-92 service-slice rule), so no other writer, engine user or mode change
  can observe its half-applied state. Paced qualification instead leaves,
  passes its checkpoint (yield, phase and total budgets) and re-enters per
  window. One absolute deadline, `SAVAGE3D_PARTIAL_CLEAR_US` = 20 ms (equal
  to the engine's execution window), bounds the whole surface; no window
  restarts it or the engine's slice. A clear that misses it fails the job
  with `-ETIMEDOUT` after restoration, without quarantine (the context then
  falls back as before). Because the boot gate runs the same full-surface
  clears under the same deadline, a mode whose bus cannot meet it refuses
  D24/S8 at boot and keeps accelerated D16.
* **The optional gates use the paced helpers.** RGB565 (4 phases) and D24/S8
  (34 phases) record `[3d] probe phase=rgb565|d24s8 index=N state=begin|end`
  and `rgb565_prepare`/`d24s8_prepare`, render only the compared rows of the
  scalar reference between checkpoints, initialize attachments with engine
  clears (the first D24/S8 clear now writes both components; no CPU upload of
  a zero shadow), and read back in restored windows. A checkpoint's budget
  timeout ends the gate and propagates through the owner; no further optional
  gate starts after a budget overrun. A numeric mismatch or a local transfer
  or partial-clear timeout, after proven restoration, refuses only that
  format, as before; the other optional gate still runs.

Research: [Linux v6.1 savage_state.c](https://github.com/torvalds/linux/blob/v6.1/drivers/gpu/drm/savage/savage_state.c),
`savage_dispatch_clear` (read in the lead's pinned
`build/downloads/f7-sources/linux-6.1-savage`), performs masked depth/stencil
clears in the engine by programming `SAVAGE_BITPLANEWTMASK` (BCI register
0xd7) around the rectangle and restoring it to all ones. That would remove
the CPU read-modify-write from every partial clear (Quake III clears depth
without stencil each frame), but it is a new register path for the fake
silicon, the restore set and the boot gate. It is not adopted here and is
recorded for a separate directive.
