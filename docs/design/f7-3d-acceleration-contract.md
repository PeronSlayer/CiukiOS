# F7: universal native 3D acceleration and libciuki3d

## Universal 3D driver model (owner amendment, 2026-10-25)

### Lead amendment 1 implementation decisions (2026-10-25)

This subsection supersedes the source-retrieval blockers below. Research uses
the lead's read-only `build/downloads/f7-sources` cache, without importing code:

| Archive | SHA-256 |
| --- | --- |
| [MesaLib-7.7.tar.bz2](https://archive.mesa3d.org/older-versions/7.x/7.7/MesaLib-7.7.tar.bz2) | `6252d7d4190b47f70d0e4ec0edbd5a70f2261c5f6e66859e5fd2572c1df6357a` |
| [MesaLib-7.11.2.tar.bz2](https://archive.mesa3d.org/older-versions/7.x/7.11.2/MesaLib-7.11.2.tar.bz2) | `57746aa8c60bc6f5596ec6c6c7274de40fa12edce5ec33fe899e704fa00b5357` |
| [xf86-video-savage-2.3.9.tar.bz2](https://www.x.org/releases/individual/driver/xf86-video-savage-2.3.9.tar.bz2) | `2c93c4db1f71e725dda0caee5821253129d4b52d7aa0fd720593e7769bceec86` |

Linux source is pinned to [v6.1](https://github.com/torvalds/linux/tree/v6.1/drivers/gpu/drm/savage).
The report records individual file hashes. Register writes follow Mesa 7.7
`savageDDInitState_s4`, `savageUpdateRegister_s4`, `savageDDDepthFunc_s4`,
`savageCalcViewport`, `savage_3d_reg.h` and `savage_bci.h`; tiled aperture
programming follows X.Org 2.3.9 `SAVAGEDRISetupTiledSurfaceRegs` and
`GetTileAperturePitch`. Linux `savage_dispatch_swap` and inline bitmap
descriptor definitions supply the conversion-copy packet. RE046-B §10 supplies
MMIO readback addresses (BCI selectors are not MMIO word offsets).

* Integer D16 is selected explicitly: float depth off, W-to-Z on, exponent
  offset zero; native LESS becomes hardware GREATER and stored depth is
  `65535 - native_depth`. Vertex Z is the reversed value divided by 65535;
  integer-only rational IEEE conversion must round ties to even. Clear and
  readback reverse the same integer representation. Mesa's floating-D16
  minimum-depth adjustment does not apply to this mode.
* Local destination/Z flush bits are set; corresponding write-low watermarks
  are zero, Z read watermarks are 24/24, destination flush is 01 and texture
  read watermark is 15. Texture addresses are re-emitted after every upload.
  Every small packet drains both engines and proves a fresh nonzero event.
  No source establishes that a COB is required for these <=32-word packets.
* SuperSavage tiled surface 0 uses `(base >> 6) | (tiles << 20) | bpp`.
  Aperture row pitch is **8192 for color, 4096 for D16**, independent of the
  physical tile pitch. Only this alias is temporarily programmed/restored;
  streams, clocks, GBD and scanout are untouched. The BAR2 mapping is an alias
  of the common owner's claimed heap, not additional memory.
* Per-resource physical storage includes padded 128-byte destination rows,
  16-row tiles and guard pages on both sides. Texture subtiles remain a
  different layout. The kernel constructs PBD/SBD inline descriptors for
  tiled-to-linear copies and restores the saved descriptor/clip state before
  releasing presenter ownership. The probe compares independent expected
  pixels/depth, conversion output and guards before enabling admission.
* The 2D lease is unchanged. The heap begins above its actual end, and ends
  below the existing cursor/firmware reserve. At 1024x768 the proposed 4 MiB
  texture heap cannot coexist with this lease. Following §5's permitted
  reduction, the initial minimum pool is **256 KiB**, after two full-size
  padded color buffers, D16, per-allocation guards and linear probe scratch.
  Failure to fit disables hardware for that mode. This is software admission
  policy, not measured capacity or a revised hardware performance gate.
* Before latency measurements, execution stays limited to <=64x64 resources,
  one triangle and one queued hardware job. A successful probe qualifies only
  that minimum; full-resolution scenes, general presentation leases and the
  broader fixed-function subset remain f7-03..06. Offscreen linear COPY never
  authorizes scanout. Mode/loss closes the attached service before revocation;
  quarantine retains every claim and mapping that may still be referenced.

The normal run-time token `3d=probe` is required for the first boot probe.
The durable key is `3d-s3savage` plus the detected vendor/device; persistence
and clearing occur outside presenter ownership. A retained marker, missing
claim, unavailable marker store or invalid mode refuses before any write.
The remaining silicon question is whether this local tiled setup works with
the retained linear display on this revision. The bounded probe is the test;
host evidence does not answer that hardware question.

The emitted minimum state image is pinned to the Mesa fields above:

| Selector | Value / fields |
| --- | --- |
| 1e | `c4000010` plus bit 27 for depth write: source ONE, destination ZERO, draw update, Gouraud, destination/Z flush |
| 20,21 | `0005f040`: point, no mips, bias 8, dMax 15, U/V clamp |
| 22,23 | Local, 8-byte-aligned owner texture base OR 2; re-emitted each draw |
| 24,25 | `00110011` MODULATE including alpha (or `00850405` without a texture), `00870407` disabled stage 1 |
| 27 | `60000000` palette size plus stage-0 enable, ARGB8888 format and log2 dimensions when textured |
| 30,31 | Fog disabled; stencil disabled, comparison ALWAYS, read/write masks ff |
| 32 | W-to-Z, integer depth, exponent zero; D16 enable for test/write, GREATER for native LESS or ALWAYS without test |
| 33,34 | Owner D16/color bases in 2 KiB units; padded width in 64/32-pixel tiles, XRGB8888 destination |
| 35,36 | Scissor starts zero, ends width/height minus one, performance bit, no cull/dither/alpha test |
| 37,38 | `00001818` Z read 24/24 and write 0/0, `4f000000` destination flush 01 / texture read 15 / destination write-low zero |

The hardware serializer also applies Mesa's `SUBPIXEL_X=-0.5` and
`SUBPIXEL_Y=-0.375` offsets. Independent host fixtures cover these bits,
the MMIO address/selector distinction, PBD/SBD descriptor ordering, texture
subtiles and rational depth rounding. The silicon probe allows ±2 channel
levels at its Gouraud sample and ±1 at its D16 sample; missing coverage,
wrong texture quadrant, any guard damage, copy mismatch or failed restore
is an error. These small interior-sample tolerances do not close H2/T1.

Heap selection measures the registry snapshot before claiming: every live
MMIO reservation, including quarantine, is excluded page-wise; an incomplete
inventory refuses. The largest remaining contiguous extent must fit the
mode proposal. For an otherwise unreserved T23 heap `[00800000,00fff000)`,
free/largest are 8,384,512 bytes and the minimum proposal is 8,183,808 bytes.
These are checked conditional values; actual boot records are authoritative.
The descriptor declares the separate 3D marker in its attempt-file list, so
the existing explicit device-keyed retry also handles an interrupted probe.

This section supersedes the Savage-specific scope of the original proposal.
The device owner, admission, copied submission ring, generation/sequence
fences, resource lifetime, VRAM reservations, presentation authorization and
loss policy belong to one family-neutral service. Display families nominate
an immutable capability descriptor in the existing display registry; a backend
provides validation, layout, packet emission, idle/flush and conversion copy.
Only a backend qualified for the current device/display generation may execute.
Recognition, potential hardware features and implemented capabilities are
different facts. Applications query implemented capabilities, never a PCI ID.
AUTO selects the scalar implementation whenever hardware is unavailable;
HARDWARE_REQUIRED returns an explicit unsupported result. No application gets
device addresses. AGP remains unavailable until a separate transport directive.

The first interface is kernel-internal (for a later opengl32 adapter), not an
addition to the public syscall or SDK ABI. Its callers are trusted adapters on
one serialized display worker; they must copy user memory before invoking it.
Worker-only entry points cannot be called directly from a syscall or IRQ.
Backend enter/leave hooks take presenter serialization before the shared engine
lock. Persistence and allocation happen outside those hooks. An adapter must
close admission and drain before mode changes/revocation; no asynchronous
presentation lease is created by an offscreen context.

The following is a research/implementation order, **not advertised support**.
Potential limits are chip-specific ceilings, not allocation promises; exact
revision-specific state needs its own directive. `B/A` means fixed-function
blend and alpha test. All unimplemented entries select software. The existing
2D PCI match tables remain authoritative, including their narrower ID sets.

| Order / display family | Primary public implementation references | Potential fixed-function subset | Submission / status |
| --- | --- | --- | --- |
| 1 s3savage | Mesa 7.7 / 7.11.2 `savage`; Linux `savage`; final DRI1 release 7.11.2 | Savage4+: 2 units, 2048 textures, D16/D24S8, B/A; earlier Savage: 1 unit | CPU BCI, optional DMA excluded; first backend, boot-probe gated |
| 2 vmwaresvga | [Gallium svga](https://docs.mesa3d.org/drivers/svga.html), Linux `vmwgfx`; maintained Gallium, no final release | SVGA3D host caps determine units/size/depth/B/A | shared FIFO/fences; not implemented, requires SVGA3D capability |
| 3 tdfx | Mesa 7.11 `tdfx`, Linux `tdfx`; [Glide3 upstream](https://github.com/sezero/glide) | Banshee 1 / Voodoo3–5 2 units, 256 / VSA-100 2048 textures, D16 (VSA-100 D24S8), B/A | MMIO/FIFO; not implemented |
| 4 rage128 | Mesa 7.11 `r128`, Linux `r128` | 2 units, 1024 textures, D16/D24, B/A | PIO/CCE DMA ring; not implemented |
| 5 radeon2d | Mesa classic `radeon`/`r200`, Linux `radeon`; 21.3 mainline then Amber | R100 3 / R200 6 units, 2048 textures, D16/D24S8, B/A | CP command ring; not implemented; R300/R4xx need separate descriptors |
| 6 matrox | Mesa 7.11 `mga`, Linux `mga` | G200 1 / G400–450 2 units, 2048 textures, D16/D24/D32, B/A | MMIO/DMA; not implemented |
| 7 intel2d | Mesa classic `i830` (historical), `i915`, Linux `i915`; 21.3 mainline then Amber for classic i915 | i830–855/865 4 units, 2048 textures, D16/D24S8, B/A | batch/ring/GTT; not implemented |
| 8 i810 | Mesa 7.11 `i810`, Linux `i810` | 2 units, 2048 textures, D16, B/A | DMA ring/GTT; not implemented |
| 9 mach64 | Mesa 7.11.2 `mach64`, historical out-of-tree DRM `mach64`, [Utah-GLX](https://utah-glx.sourceforge.net/) | Rage Pro: Mesa exposes 2 stages, 1024 textures, D16, B/A; one uploaded mip level | MMIO/GUI FIFO/DMA; not implemented; chips without triangle setup stay software |
| 10 unichrome | Mesa 7.11.2 `unichrome`, Linux `via` | 2 units, default 512 textures (1024 with excess_mipmap), D16/D24S8/D32, B/A | MMIO/AGP command regulator; not implemented |
| 11 sis | Mesa 7.11.2 `sis`, Linux `sis` | SiS300 2 units, 1024 textures, D16/D24/D32, B/A; 315 is not covered by this driver | MMIO/AGP queue; neither generation implemented |
| 12 nv2d | Mesa classic `nouveau` nv04/nv10/nv20, Linux `nouveau`; 21.3 mainline then Amber | TNT 2 / GeForce1–2 2 / GeForce3–4 4 units; 1024/2048/4096 textures, D16/D24S8, B/A | PFIFO PIO/DMA push buffers; software-only under f7-26, complete PIO 3D/context lifecycle not established ([evidence](f7-family-nvidia-nv04-nv20.md)) |
| 13 riva128 | [Utah-GLX](https://utah-glx.sourceforge.net/), NVIDIA public NV3 class references; no Mesa classic DRI shipping claim | 1 unit, 1024 textures, D16, B/A restrictions need qualification | PIO FIFO; not implemented |
| 14 i740 | Utah-GLX i740; no supported Mesa classic DRI/Linux DRM pairing established here | 1 unit, 1024 textures, D16, B/A restrictions need qualification | MMIO/FIFO/AGP; not implemented |
| 15 s3virge | historical Mesa `s3v` / Utah-GLX; no maintained Linux DRM module; last release not established | 1 unit, 128/256 texture limits depend on revision, D16, restricted B/A | MMIO FIFO; not implemented |
| 16 trident | historical experimental Mesa `trident`; no production DRM pairing or final shipping version established | Blade3D/CyberBlade-specific limits not established; no features advertised | MMIO; not implemented |
| — cirrus, chipshiqv, neomagic, s3trio64, s3legacy | existing 2D families | software-only for these matched adapters | no 3D backend |

Mesa's [8.0 removal list](https://docs.mesa3d.org/relnotes/8.0.html)
establishes the DRI1 boundary; the last maintenance release is
[7.11.2](https://docs.mesa3d.org/relnotes/7.11.2.html).
[Amber](https://docs.mesa3d.org/amber.html) establishes the later classic
driver boundary. Uncertain historical limits above remain research items,
not defaults to use in code. The conservative descriptors advertise zero
hardware features until a family backend has its own evidence.

#### Family matrix source audit from the local 7.11.2 archive

The archive pin above fixes these file/function references. `MaxTextureLevels`
includes level zero: 11 levels means a 1024 side, 12 means 2048. For
`driCalculateMaxTextureLevels` the dimension argument is a log2 side instead.
Reported software stages do not prove simultaneous physical texture units.

| Families | Inspected 7.11.2 evidence / release boundary |
| --- | --- |
| savage | `savage/savage_xmesa.c:savageCreateContext` (one/two units, dimension exponent 11, D16/D24); integer/float and tiling distinctions checked against 7.7. Final 7.11.2, removed for 8.0. |
| tdfx | `tdfx/tdfx_context.c` initialization: Banshee one / others two, levels 9 versus Voodoo4/5 12, D16/D24 visuals; `tdfx_screen.c`, Glide3 calls. Final 7.11.2. |
| rage128 | `r128/r128_context.c`: two units, dimension exponent 10; `r128_screen.c`/`r128_state.c` for visual/depth and CCE submissions. Final 7.11.2. |
| radeon2d | `radeon/radeon_context.c` and `r200/r200_context.c`: limits/options and 12 levels; three/six maximum units in their context headers. Both remain through mainline 21.3 then Amber. These sources do not qualify later Radeon chips matched by the 2D family. |
| matrox | `mga/mga_xmesa.c`: G200 one / G400 two, dimension exponent 11, explicit 16/24/32 depth branches; `mgastate.c` DMA. Final 7.11.2. |
| intel2d | `i915/i830_context.c`: four units, 12 levels and 2048 rectangle side; i830 hardware support is inside the i915 directory in this archive. `intel_batchbuffer.c`/`intel_screen.c` submit through DRM i915. Classic driver persists through 21.3 then Amber. |
| i810 | `i810/i810context.c`: two units, dimension exponent 11; screen/depth and DMA ioctl paths in `i810screen.c`/`i810ioctl.c`. Final 7.11.2. |
| mach64 | `mach64/mach64_context.c`: **two exposed stages**, dimension exponent 10, only one uploaded mip level; `mach64_texstate.c` multitexture state. The upstream DRI project limits support to triangle-setup Rage Pro derivatives; DRM mach64 was out-of-tree, not a mainline Linux module. Final 7.11.2. |
| unichrome | `unichrome/via_context.c`: levels 10 by default, 11 with `excess_mipmap`, two units, D16/24/32 cases; MMIO/AGP regulator in `via_ioctl.c`. Final 7.11.2. |
| sis | `sis/sis_context.c`: 6326 one/9 levels, other supported chips two/11 levels. `sis_screen.c` and `sis_state.c` do not establish SiS315 support. Final 7.11.2 for this driver, not a release claim for 315. |
| nv2d | `nouveau/nv04_context.c`: 11 levels; `nv10_context.c`: 12 levels; `nv20_context.c` inherits common limits and uses four units. `nv04/10/20_driver.h`: 2/2/4 units. NV20's nominal 4096 hardware limit is not advertised by this matrix as a verified 7.11.2 driver limit. PFIFO buffer submission uses nouveau. Classic NV04–20 persists through 21.3 then Amber. |
| riva128, i740, s3virge, trident | No corresponding implementation is shipped in the inspected 7.11.2 DRI directory. Utah-GLX / historical Mesa references in the main matrix remain starting points; final shipping release and revision-specific feature limits are **not established** by this archive. They remain NOT_IMPLEMENTED. |
| vmwaresvga | Gallium `svga` and Linux `vmwgfx`, not a 7.11.2 classic DRI driver. Query SVGA3D caps; do not use fixed host-independent limits or imply VirtualBox supports every VMware command. |

Release boundaries were cross-checked against Mesa's
[8.0 removals](https://docs.mesa3d.org/relnotes/8.0.html) and
[Amber list](https://docs.mesa3d.org/amber.html). Mach64's module/triangle-setup
scope is also documented by the upstream
[DRI project](https://dri.freedesktop.org/wiki/ATIMach64/).
The listed 7.11.2 files are research references, never kernel dependencies.

### f7-02 pre-write findings and gates (2026-10-25)

* `SavageSetGBD_PM` uses `TILE_DESTINATION` for **both** pixel sizes,
  surface pitch in bits starting at 20 and base shifted by 6. The ordinary
  Savage4/Twister recipe uses pitch starting at 24 and a byte base. Do not
  substitute that recipe. The function also changes primary streams and
  blanks scanout: it cannot be copied as an offscreen initialization routine.
  [X-A](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_accel.c).
* `SavageMapMem` chooses BAR2 on SuperSavage. The alias is not extra VRAM.
  `SavagePreInit` disables both DMA modes and defaults to disabled tiling,
  referencing bug 805380. Offscreen tiling plus linear scanout remains a
  **silicon gate**, not an established supported combination.
  [X-D](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_driver.c).
* Mesa `savageChooseVertexFormat_s4` orders X,Y,Z,W,diffuse,specular,U0,V0,U1,V1.
  Linux skip flags 08h (specular) and C0h (stage 1) give seven DWORDs for one
  texture, or a 22-DWORD triangle packet. With wait+event this is 24, within
  32 on-chip words. The serializer uses integer round-to-nearest-even IEEE
  conversion, with validated coordinates/depth/positive reciprocal W.
  [M-V](https://chromium.googlesource.com/chromium/deps/mesa/+/3a0db227ffe90888ad760c61a63226988c974e00/src/mesa/drivers/dri/savage/savagetris.c),
  [L-H](https://sources.debian.org/src/linux/6.1.4-1/drivers/gpu/drm/savage/savage_drv.h).
* Mesa reverses depth comparison in `savageDDDepthFunc_s4`; its viewport
  conversion is essential. A source-backed bit-exact D16 initialization
  requires the complete `savage_3d_reg.h` and viewport implementation.
  The header and `savageioctl.c` could not be retrieved in this environment
  (web cache misses; shell DNS unavailable). No guessed depth/control words
  are emitted. [M-S](https://chromium.googlesource.com/chromium/deps/mesa/+/6da96107e2467063c72e1ec5804f0618a6ce83d3/src/mesa/drivers/dri/savage/savagestate.c).
* Mesa forces texture address re-emission after upload and sets destination/
  Z flush controls and watermarks; WAIT/event alone does not establish cache
  flush. Linux's fake-DMA path is not proof of a cache-flushed no-COB 3D path.
  Cache programming remains blocked on those complete register definitions.
* The existing qualified idle signature, read cap, timeout, dead-MMIO and
  event checks remain authoritative. 3D packets additionally wait for both
  engines before publishing an event. Sequence fences are software 64-bit
  values, independent of 16-bit event wrap; zero/stale events never complete.
* DRM `savage_dispatch_swap` is a blit, not a flip. It does not prove that a
  SuperSavage tiled source can be copied into the retained linear scanout.
  Qualify conversion into a separate offscreen **linear** scratch target
  first, with guards and known pixels. The initial diagnostic must not touch
  scanout, stream controls, clocks, COB or PCI bus mastering.
  [L-S](https://kernel.googlesource.com/pub/scm/linux/kernel/git/stable/linux-stable.git/+/a4da411e41892628f35aa180a78b7b0f8f1e3038/drivers/gpu/drm/savage/savage_state.c).

The bounded diagnostic must refuse before its first write if any source,
reservation, durable marker, shared-owner, status or alias prerequisite is
missing. The report distinguishes that refusal from a successful triangle
probe. Host fixtures establish transport/layout behavior only. Hardware
activation must stay off until the missing definitions and T23 evidence close
these gates; this amendment does not waive §9's pre-write requirement.

### Internal minimum implementation boundary (2026-10-25)

`drivers/video/render3d.h` is the worker-only adapter interface, not an SDK
wire definition. `display_driver.render3d` nominates a family descriptor;
`render3d_init_family` consumes that nomination without another PCI claim.
Not-implemented nominations and an unqualified first backend advertise zero
hardware features. The second fake family exercises the same nomination path.
The existing 2D registration order, mappings and activation remain authoritative.

The minimum implements Gouraud lists, LESS/D16 test and independent write mask,
one nearest/clamp ARGB8888 MODULATE texture, color/depth clear and copy into
another owned offscreen color resource. COPY does **not** authorize scanout.
Other states are not expressible in this internal version. Coordinates are
Q16.16, Z is integer 0..65535, reciprocal W is Q16.16 in 1/256..256 and U/V
are Q16.16 in 0..1. Pixel centers are half-integers; top/left edges include
samples and bottom/right edges exclude them. D16 interpolation and channels
round half upward; hardware IEEE serialization rounds ties to even.

At 1024 maximum dimension an edge product is bounded by 2^52. Barycentric
fractions use sixteen bounded remainder steps instead of a potentially
overflowing shifted edge numerator. Weighted reciprocal W/UV values remain
below 2^41; perspective recovery remains below 2^40 before division. Scalar
draw slices cover at most four rows (4096 candidate pixels); clear/copy
slices cover sixteen rows. Admission bounds total triangle bounding-box work
to 65536 pixels and lists to 64 triangles; unsupported larger submissions
return EAGAIN before side effects. The worker rotates between slices while
preserving a context's own job order. These are CPU work bounds, not measured
millisecond throughput claims.

The staging ring is exactly 256 KiB, four 64 KiB slots, two jobs/context.
There are at most 32 contexts, four/process, and 256 resources/context.
The 32 MiB service ceiling includes the fixed service/ring plus lazily charged
resource metadata and canonical shadows. Pending frees and quarantined
allocations remain charged. Handles never wrap/recycle; exhaustion fails.
Fences carry device generation, device-monotonic sequence and a process-owned
context/consecutive receipt ordinal, so completed receipts survive ring reuse.
For a pending receipt, sequence and ordinal must match the queued snapshot.
After completion, identity is generation + owned context + ordinal; sequence
is informational and is ignored, including zero or a different context's
sequence. A completed ordinal denotes an actual submission to that context,
not an authenticated device-sequence/ordinal pair. Invalid generations,
foreign contexts and zero/unsubmitted ordinals remain errors. This bounded
history rule avoids retaining an unbounded sequence map per context.
Waits cap caller timeout at 100 ms and use an independent 1024-observation
bound for a frozen clock. The caller supplies the worker's sleep/yield hook;
no wait primitive is exposed directly to an untrusted caller.

Unmeasured hardware is limited to one queued <=64x64 target job, one triangle
and <=4096 candidate pixels; backend validation can refuse further. The
shared packet transport uses the existing idle signature/deadline/read cap,
both-engine WAIT and nonzero event tag. Qualification is false on every
normal activation. With `3d=probe`, the existing owner can claim a disjoint
heap/BAR2 alias, persist the operation marker, execute the positive guarded
probe and enable only the bounded minimum after successful marker clearing.
An unavailable prerequisite refuses without programming. Engine/guard/restore
failure quarantines both domains and retains the marker, allocations and
claims. `render3d_native_init` attaches an internal consumer through the bound
display descriptor; it selects software when no qualified backend exists.
COPY uses an owned `R3D_LINEAR_COLOR` buffer and canonical readback; it grants
no scanout lease. Normal mode/native-display revocation closes admission and
cancels pending work before the common owner's mappings are released.
Actual delegated full-screen presentation remains f7-04.

#### Lead amendment 2 review fixes (2026-10-25)

Close marks every live context dying **and every resource pending-free before
cancelling queued jobs**. Cancellation may collect a context immediately;
its shadows must be reclaimable before that collection drops the metadata.
Quarantined hardware allocations still remain charged and retained.

The completed-receipt rule above is the lead's explicitly permitted ordinal-only
alternative. Linux v6.1 [`savage_bci_event_wait` and `savage_reclaim_buffers`](https://github.com/torvalds/linux/blob/v6.1/drivers/gpu/drm/savage/savage_bci.c)
compare monotonic event progress and age reclaimed buffers after a 3D wait;
they do not authenticate a process-specific software receipt. Ciuki's ordinal
ownership is its own service policy, separate from hardware event completion.

The fake packet interpreter now needs to decode scissor dimensions, padded
color/depth pitches, depth enable/write/compare and texture enable/dimensions
instead of asserting the one probe state. Fixtures exercise all 63 nonzero
flag combinations, empty lists where permitted, odd/tile-boundary target sizes
and every admitted power-of-two texture size. Expected pixels/depth are checked
independently of the production scalar renderer. The reference fields are
Mesa 7.7 `savageDDDepthFunc_s4`, `savage_3d_reg.h`, `tileInfo_pro` and X.Org
2.3.9 `SAVAGEDRISetupTiledSurfaceRegs`/`GetTileAperturePitch`, pinned above.
No additional register writes, hardware caps or silicon qualification follow
from this host-model coverage.

### f7-14 implementation note (2026-10-26): native API and Graphics Demo consumer

[f7-14](../directives/f7-14-native-gl-hardware.md) implements the native half
of section 7 and the f7-05 consumer with the native OpenGL library instead of
a separate `ciuki3d_` API: `libgl` builds the opengl32 engine, journal,
admission and provider sources, so native and Win32 programs share one GL
semantics, capability snapshot and whole-context fallback (7.2). The backend
policy of 7.1 is `ciuki_gl_set_backend` (automatic, software, hardware with
`-ENODEV` when unavailable); completion accounting is `ciuki_gl_status`;
full screen is the common lease of section 6 (v2 `DESK_LEASE`, kernel grant
to the desktop's channel peer) presented by `ciuki_gl_present` as `gpu-copy`
or `cpu-copy`. Graphics Demo gains the backend, native-resolution and
full-screen controls and logs completed-copy FPS with GPU/CPU frame counts,
refusals and fallbacks. Decisions and evidence:
[opengl-software.md](opengl-software.md), [graphics-demo.md](graphics-demo.md),
[f7-14 validation](../validation/f7-14-native-gl.md). The T2 textured scene
(floor, wall, eight cubes) is not part of f7-14.

Research and repository comparison: **2026-10-14**. Directive:
[f7-00](../directives/f7-00-3d-acceleration-contract.md). Status: original research contract; the amendments above record the f7-02 implementation
for lead review. This document contains no hardware qualification or performance
result. The later F7 directives and hardware gates remain separate.

The outcome is a native triangle renderer on the T23's **5333:8c2e
SuperSavage IX/C SDR**, shared safely with the existing 2D engine, and a
native library with the same rendering semantics on generic VBE hardware.
Graphics Demo is the first consumer. F5 can subsequently map Direct3D 6/7
immediate mode and OpenGL 1.1 onto it. Passing a native demo does not establish
complete DirectX or OpenGL compatibility.

## 1. Sources, provenance and evidence boundaries

These are original specifications or upstream source, including preserved
copies. Links to mutable upstream mirrors identify the functions inspected;
implementation directives must pin their retrieved revisions and retain
required notices for any reused code. No upstream code or register manual
is imported by this task.

| Reference | Primary source and inspected material |
| --- | --- |
| S3-R | S3, [Savage4 Registers, RE046-B, April 1999](https://vgamuseum.info/images/doc/s3/savage4_registers.pdf): §1 addressing/BCI; §8 streams; §10 pipeline; §12 mastered transfers; §13 status. Printed p. 1 is PDF page 5 (zero-based page 4). |
| IBM-T | IBM, [ThinkPad Personal Systems Reference, withdrawn A/T/X/G models](https://psref.lenovo.com/syspool/Sys/PDF/withdrawnbook/tawbook.pdf), T23 sheets (PDF pages 34–40): IXC16, AGP 4X, 16 MB SDRAM. |
| S3-M | S3 Graphics, [SuperSavage/IXc-166 announcement, 2002-07-24, reproduced by HEXUS](https://m.hexus.net/tech/items/graphics/1880-selects-s3-graphics-supersavage-top-line-p4-tecra-systems/?print=1): 16 MB multichip module (MCM). This is a different notebook SKU, not the T23's clock specification. |
| M-P | [Mesa 7.7 import provenance](https://chromium.googlesource.com/chromium/deps/mesa/+/3a0db227ffe90888ad760c61a63226988c974e00): the import states its MesaLib 7.7 origin and that only README.chromium was added. This fixes the historical Mesa version examined here. |
| M-C | Mesa 7.7, [savage_xmesa.c](https://chromium.googlesource.com/chromium/deps/mesa/+/3a0db227ffe90888ad760c61a63226988c974e00/src/mesa/drivers/dri/savage/savage_xmesa.c), `savageCreateContext`, `savage_pipeline`: CPU T&L, texture limits, depth formats, SuperSavage vertex-DMA exclusion. |
| M-T | Mesa 7.7, [savagetex.c](https://chromium.googlesource.com/chromium/deps/mesa/+/3a0db227ffe90888ad760c61a63226988c974e00/src/mesa/drivers/dri/savage/savagetex.c): `tileInfo_pro`, `tileInfo_s3d_s4`, `savageSetTexImages`, upload/filter/state paths. |
| M-V | Mesa 7.7, [savagetris.c](https://chromium.googlesource.com/chromium/deps/mesa/+/3a0db227ffe90888ad760c61a63226988c974e00/src/mesa/drivers/dri/savage/savagetris.c), `savageChooseVertexFormat_s4`, `savageRenderStart`: attributes, skip flags and software fallback. |
| L-H | Linux 6.1.4, [savage_drv.h](https://sources.debian.org/src/linux/6.1.4-1/drivers/gpu/drm/savage/savage_drv.h): BCI commands/selectors, on-chip FIFO, tiled descriptors. |
| L-B | Linux 6.1.4, [savage_bci.c](https://sources.debian.org/src/linux/6.1.4-1/drivers/gpu/drm/savage/savage_bci.c), `savage_dma_flush`, `savage_fake_dma_flush`, `savage_do_init_bci`, event emit/wait: command transport and lifetime. |
| L-S | Linux 6.1.4, [savage_state.c](https://sources.debian.org/src/linux/6.1.4-1/drivers/gpu/drm/savage/savage_state.c), state validation, clip emission, `savage_dispatch_swap`: address checks, global-state synchronization, swap by blit. |
| L-L | Linux 6.1.4, [savage_drv.c](https://sources.debian.org/src/linux/6.1.4-1/drivers/gpu/drm/savage/savage_drv.c): module licence and per-file permission notice. |
| X-D | XFree86/X.Org Savage, [savage_driver.c, NetBSD upstream distribution mirror](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_driver.c), `SavageMapMem`, `SavagePreInit`, `SavageAdjustFrame`: generation-specific mappings, DMA/tiling exclusions, display-head addressing. |
| X-A | Same driver, [savage_accel.c](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_accel.c), `SavageSetGBD_PM`, `SavageSetGBD_Twister`: SuperSavage surface descriptors, tile/stream controls and IGA selection. |
| GL-1 | Khronos registry, [OpenGL 1.1 specification](https://registry.khronos.org/OpenGL/specs/gl/glspec11.pdf), §§2, 3.5, 3.8, 4.1: transformations, rasterization, texture sampling and fragment operations. |
| GL-T | Khronos registry, [EXT_texture_compression_s3tc](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_texture_compression_s3tc.txt): DXT1/3/5 block formats. |
| DX-I | Wine 10.0 upstream, [d3d.h](https://raw.githubusercontent.com/wine-mirror/wine/wine-10.0/include/d3d.h) (`IDirect3DDevice3/7`, vertex buffers) and [d3dtypes.h](https://raw.githubusercontent.com/wine-mirror/wine/wine-10.0/include/d3dtypes.h) (`D3DTLVERTEX`, FVF and render/texture-stage enums). These preserve the older interfaces; they are not a Direct3D 9 substitute. |

The Mesa files carry VIA/S3 MIT-style permission notices. Linux's module
identifies itself as `GPL and additional rights`; the Savage files also carry
Felix Kuehling's permissive notices. Kernel integration remains GPL-2.0-only;
the native SDK/library should follow the existing SDK's MIT licence. Do not
move Linux kernel dependencies into the user library. Any later reuse requires
the actual file's notice, not a blanket assumption about a whole project.
Wine's interface references are LGPL-2.1-or-later. [M-C](https://chromium.googlesource.com/chromium/deps/mesa/+/3a0db227ffe90888ad760c61a63226988c974e00/src/mesa/drivers/dri/savage/savage_xmesa.c),
[L-L](https://sources.debian.org/src/linux/6.1.4-1/drivers/gpu/drm/savage/savage_drv.c),
[DX-I](https://raw.githubusercontent.com/wine-mirror/wine/wine-10.0/include/d3d.h).

RE046-B describes **Savage4**, not every SuperSavage revision. Its register
descriptions are a starting point; the SuperSavage branches of the upstream
drivers and T23 measurements decide generation-specific behavior. No public
SuperSavage register reference was located. Some historical files were
accessible only through indexed source excerpts; these are identified by
function above, and missing details must be resolved before their first
hardware write. The initial research lacked the complete Mesa register header; lead amendment 1
supplies the pinned full files and supersedes that retrieval limitation.

## 2. Existing repository behavior and required integration

| Existing implementation | Consequence for F7 |
| --- | --- |
| `src/kernel/drivers/savage2d.c:savage2d_start/submit`, `src/kernel/include/ciuki/savage2d.h` | One BAR0 register owner, VGA accesses through claimed ports, deferred durable attempt marker, verified reset before status, UC BCI writes, synchronous completion, no DMA/COB/shadow status. Reuse this owner and failure policy. |
| Same driver's cursor allocation | Last firmware-reported VRAM page may already belong to the cursor. F7 must reserve it explicitly; fbdev's visible `size` is not all VRAM. |
| `src/kernel/drivers/fbdev.c`, `src/kernel/include/ciuki/fbdev.h` | Presenter mutex, display generation, WC write drain, mode transaction and CPU-access safety govern both paths. F7 cannot create an independent presenter. |
| `src/kernel/proc/grants.c`, `src/kernel/core/display.c` | Desktop owns the non-inheritable display grant and mode authority. There is no delegated 3D presentation lease in this worktree. |
| `apps/gfxdemo/scene.c`, `apps/gfxdemo/app.c`, [graphics-demo.md](graphics-demo.md) | Existing 320×240 fixed-point cube uses reciprocal depth and CPU texturing. Scaling it to 1024×768 is not a native-resolution F7 pass. Migrate the scene to the common API, with an explicit normalized-depth conversion. |
| [win32-personality-contract.md](win32-personality-contract.md), full-screen exclusive mode and Q5 decision | F5 already specifies desktop-delegated presentation, synchronous revocation, no client framebuffer mapping and restoration within 2 s. F7 extends that same lease, with no second full-screen authority. |

The amendments in [savage-2d.md](savage-2d.md) for f1-27/f1-49 remain binding
until explicitly superseded by a reviewed F7 implementation directive. The
current ports-only transport and actual code readback rules take precedence
over an older initialization recipe. F7 must keep the existing 2D/cursor,
display-mode, desktop and Graphics Demo regressions passing.

## 3. Hardware facts and programming model

### 3.1 SuperSavage is a separate qualified target

IBM specifies dedicated 16 MB SDRAM on the T23. The IX/C integrated-memory
designation means an on-package MCM configuration, not permission to allocate
arbitrary system RAM as VRAM. It must not be confused with a ProSavage UMA
northbridge. Runtime firmware VRAM reporting and resource reservations still
bound every allocation. [IBM-T](https://psref.lenovo.com/syspool/Sys/PDF/withdrawnbook/tawbook.pdf#page=34),
[S3-M](https://m.hexus.net/tech/items/graphics/1880-selects-s3-graphics-supersavage-top-line-p4-tecra-systems/?print=1).

X.Org has separate **IGA1/IGA2 display heads** on SuperSavage. Mesa exposes
**two texture units on Savage4 and later**, including SuperSavage. These are
different meanings of "two pipes"; the examined primary material does not
establish two independent pixel rasterizers or two pixels per clock. The
contract must not budget that throughput. F7 initially qualifies one active
display head and two texture stages; second-head support is deferred.
[X-A](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_accel.c),
[M-C](https://chromium.googlesource.com/chromium/deps/mesa/+/3a0db227ffe90888ad760c61a63226988c974e00/src/mesa/drivers/dri/savage/savage_xmesa.c).

The engine accepts already transformed/lit vertices. Mesa performs transforms,
lighting, fog coordinates and texture transforms on the CPU. No hardware T&L,
vertex shaders or pixel shaders may be advertised. Mesa's 2D texture ceiling
is 2048×2048, subject to heap space; it excludes volume, cube and rectangle
textures. One 2048² RGBA image alone takes 16 MiB, before mipmaps or render
buffers, so that ceiling is not a usable T23 residency promise.
[M-C](https://chromium.googlesource.com/chromium/deps/mesa/+/3a0db227ffe90888ad760c61a63226988c974e00/src/mesa/drivers/dri/savage/savage_xmesa.c).

Initial hardware activation is limited to T23 **5333:8c2e** and separately
qualified **Savage4 5333:8a22**. Detection must include PCI revision, BARs,
firmware mode, format, pitch and safe resource extents. Other SuperSavage IDs
already eligible for 2D do not automatically gain 3D. Savage3D/MX/IX,
Savage2000 and ProSavage retain software 3D until separate directives.

### 3.2 Addressing and BCI command submission

SuperSavage uses BAR0 + 0 for registers, BAR1 for the linear framebuffer and
**BAR2 for the CPU tiled aperture**. Ordinary Savage4's tiled aperture starts
at framebuffer base + 32 MiB. Aperture windows alias VRAM; their mapped length
is not extra physical video memory. Validate all BAR extents without destructive
BAR sizing or relocation. [X-D, SavageMapMem](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_driver.c).

The BCI is a word stream, not an autonomous modern GPU ring. Relevant packet
headers from the DRM definitions are:

| Command | Header and payload rule |
| --- | --- |
| Set registers | `0x96000000`, count in bits 23:16, first BCI register selector in the low 16 bits, followed by that many values. |
| Draw primitive | `0x80000000`, vertex count in bits 23:16; list/strip/fan modifiers and attribute-skip bits; followed by the selected vertex words. |
| Wait | `0xc0000000`; 3D wait `0x00010000`, 2D wait `0x00020000`. |
| Event tag | `0x98000000` with a 16-bit tag; software must track wrap. |
| Command DMA | `0xa8000000`; encoded count is QWords minus one. Deferred on T23. |

The on-chip FIFO has 32 DWORD entries. The checked vertex path orders
position/depth/reciprocal-W, diffuse/specular color and up to two UV pairs;
skip flags determine which words are present. A change of format flushes the
old primitive before another layout is emitted. [L-H](https://sources.debian.org/src/linux/6.1.4-1/drivers/gpu/drm/savage/savage_drv.h),
[M-V](https://chromium.googlesource.com/chromium/deps/mesa/+/3a0db227ffe90888ad760c61a63226988c974e00/src/mesa/drivers/dri/savage/savagetris.c).

**Decision:** start with a kernel-owned software submission ring and MMIO
stores to BAR0's BCI window at `0x10000`. Keep COB, shadow status, IRQ-driven
completion, PCI bus-master activation and AGP/GART allocation disabled. Each
complete packet must fit the proven FIFO capacity, including its header.
Expand strips/fans/indices into bounded triangle-list packets initially.
Record the exact qualified vertex word order and skip combination in f7-02;
do not guess it from a byte stride or mix Savage3D and Savage4 formats.

Wait for a safe FIFO/completion condition before reuse. Initially retain the
existing combined idle/empty checks around small packets; f7-06 may batch more
aggressively only after a generation-specific FIFO status model is verified.
DRM's larger FIFO-used mask must not replace Ciuki's selected idle layout
without checking the T23's raw status and revision. UC register mapping,
CPU WC drains, posted-write ordering and the frozen-clock poll cap remain
mandatory. No kernel floating-point operation is permitted: conversion of
validated fixed-point vertices to hardware IEEE single-precision bit patterns
must use integer code with host-tested rounding and range handling.

### 3.3 State register map

This is a register-family map, not a complete initialization script. S3-R
§10 distinguishes local per-triangle state from global state requiring an
idle engine. The offset/selector pairs below match L-H's Savage4 selectors.
Reserved fields and revision-dependent depth controls require explicit masks.
[S3-R](https://vgamuseum.info/images/doc/s3/savage4_registers.pdf#page=191),
[L-H](https://sources.debian.org/src/linux/6.1.4-1/drivers/gpu/drm/savage/savage_drv.h).

| Pipeline state | MMIO offset / BCI selector | Role |
| --- | --- | --- |
| Draw local | `48584 / 1e` | Source/destination blending and write controls. |
| Texture controls 0/1 | `4858c/48590 / 20/21` | Filtering, mip and addressing controls. |
| Texture addresses 0/1 | `48594/48598 / 22/23` | Device-owned texture bases and memory-location flags. |
| Texture blending 0/1 | `4859c/485a0 / 24/25` | Fixed-function combine stages. |
| Texture descriptor | `485a8 / 27` | Formats, dimensions and stage enables. |
| Fog / stencil | `485cc/485d0 / 30/31` | Deferred capabilities. |
| Depth control / offset | `485d4/485d8 / 32/33` | Comparison, depth mode, base and width. |
| Destination | `485dc / 34` | Target base, width and format. |
| Draw controls 0/1 | `485e0/485e4 / 35/36` | Scissor and alpha-test controls. |

**Decision:** f7-03 starts with RGB565/XRGB8888 targets and D16 depth;
D24S8, fog and advanced combiners stay unavailable until separately tested.
Store logical state per context and emit a complete approved state image on
every context/2D-to-3D transition. Any change of a global register drains the
older work first. Flush 3D writes before a 2D copy, clear or CPU read/upload;
flush CPU texture uploads before a draw. Reset dirty-state tracking after a
mode change, loss or quarantine. A state cache must never let a client inherit
another context's target, scissor, texture or blend setting.

### 3.4 Tiling, texture layout and DMA limits

Mesa selects a different destination/texture tile layout for chips later than
Savage4 (`tileInfo_pro`). Its uploads include subtiles, packed small mip levels
and compressed blocks. DXT1 uses 8-byte 4×4 blocks; DXT3/5 use 16-byte blocks.
Linear application rows cannot simply be copied into a hardware texture heap.
[M-T](https://chromium.googlesource.com/chromium/deps/mesa/+/3a0db227ffe90888ad760c61a63226988c974e00/src/mesa/drivers/dri/savage/savagetex.c),
[GL-T](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_texture_compression_s3tc.txt).

**Decision:** use per-generation pack/unpack helpers with independent layout
fixtures. Keep a canonical RAM texture image; validate every mip's full padded
extent. Start with power-of-two RGB565/RGBA8888 textures, nearest/bilinear
filtering and repeat/clamp-to-edge. The Ciuki texture remains a deterministic
conversion of `assets/brand/ciuki-logo.png`; no regenerated mascot or lossy
substitute is required. DXT1/3/5 decode/upload is a later optional capability
within f7-03, enabled only after known-block tests; compression encoding is
not required. Unsupported border/filter/combine behavior selects software.

X.Org disables **both command and vertex DMA** on SuperSavage. Its newer
default also disables SuperSavage tiling with a reference to a reported bug.
These exclusions forbid assuming that a generic DRI setup is safe on the T23.
[X-D, SavagePreInit](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_driver.c).

DRM provides command-DMA and CPU-fed "fake DMA" paths. Its real command-DMA
path pads to even DWORD counts, aligns the next block to eight DWORDs, tracks
page age with event tags, and treats command and vertex DMA as exclusive
configurations. That is a transport reference, not evidence that T23 DMA works.
[L-B](https://sources.debian.org/src/linux/6.1.4-1/drivers/gpu/drm/savage/savage_bci.c).

**Decision:** no physical DMA in f7-01…f7-06. A later Savage4 DMA/AGP directive
must declare address masks, contiguity, alignment, GART/host-bridge ownership,
cache ordering, pinned allocation budgets and proven shutdown, following
[device-firmware-ownership.md](device-firmware-ownership.md). SuperSavage DMA
requires new erratum evidence and explicit requalification. AGP 4X in IBM's
specification does not override this restriction. Offscreen tiled rendering
and its copy-out must be qualified before enabling the native backend; if
either fails, report software 3D and keep the hardware F7 gate open.

## 4. Kernel ownership, resources and bounded execution

### 4.1 One device owner

Extend the existing Savage owner into a common 2D/3D engine service. There must
be one registry claim for BAR0 and the VGA ports, one activation/reset path,
one quarantine state and one serialized hardware writer. 2D, cursor, 3D and
presentation are clients of that service, not competing drivers. Claims for
offscreen VRAM and BAR2 aliases must identify the same device generation and
underlying allocations; aliases must not bypass overlap checks.

Activation preserves the durable attempt-marker ordering and verified reset
of f1-27/f1-49. Persist markers outside graphics/VFS-incompatible locks; no
file I/O under the presenter/engine mutex. A new first 3D/tiling attempt needs
a durable operation marker under the same failed-attempt policy, even when
2D is already active. An uncleared 3D marker blocks further 3D activation;
an engine-domain failure must also block unsafe 2D reactivation. No context
creation may reset working hardware or clear a failed-attempt marker.

Worker execution takes the presenter serialization before the engine lock;
no engine-to-presenter inversion. Copy/validate requests and allocate staging
memory before taking those locks. Mode changes and lease revocation close
the submission gate first, then quiesce through that same worker. No wait
holds interrupts disabled, a spinlock, or a filesystem/storage lock. Cursor
movement cannot write through an in-flight global-state transition.

### 4.2 Submission ring, fences and admission

The following are **proposed software limits**, not chip specifications.
f7-01 must freeze their ABI/numeric proofs; f7-06 may tune them only with
recorded latency and memory evidence.

- One global **256 KiB RAM command/staging ring**, at most four queued jobs;
  maximum copied job size 64 KiB including state, vertices and references.
  At most two jobs per context. Saturation returns `-EAGAIN` without taking
  a partial job. The full job is validated before any hardware side effect.
- At most 32 live contexts globally and four per process, with lazy target
  allocations; maximum 256 live resources per context. Budget accounting
  includes copied/pinned requests, pending frees and quarantine. Context
  creation cannot reserve four screen-sized sets per process implicitly.
- Initially yield between at most 64 triangles or 1 ms of worker CPU time,
  whichever comes first. A bounded slice drains the engine before giving
  another context or 2D request its turn. Present/revoke/control work has
  priority at those boundaries. Large clears/blits also split into bounded
  strips; triangle area/overdraw must be charged, not just vertex count.
- Bound total queued estimated work to a **20 ms service window** using
  conservative qualified costs. Before those measurements, permit only one
  outstanding hardware slice and conservative pixel bounds. Reject excess
  work with `-EAGAIN`; never claim that a 64 KiB byte limit proves 20 ms.
- Each hardware wait retains a monotonic deadline of at most **20 ms** and
  the existing independent frozen-clock read cap. A fence wait exposes a
  caller timeout capped at 100 ms and sleeps with interrupts enabled.
  Caller timeout alone does not establish an engine fault or completion.

Use a software fence consisting of device generation plus a monotonically
increasing sequence. Never recycle handles or resources still referenced by
queued work. The initial completion proof is the qualified idle/empty path.
If event tags are later used, reserve tag zero, track 16-bit wrap, keep the
outstanding window below half the tag range, and test wrap/reset/stale-tag
behavior. An event-tag update alone is not proof of finished rendering:
required 2D/3D waits must precede it. DRM orders those waits in its event
emission path. [L-B](https://sources.debian.org/src/linux/6.1.4-1/drivers/gpu/drm/savage/savage_bci.c).

CPU scheduling and completed bus accesses are bounded by these rules. A PCI
read that hard-locks the bus cannot be interrupted by a software deadline.
The attempt marker provides next-boot containment; the contract cannot promise
that the current desktop survives that failure.

### 4.3 Untrusted clients

Clients send typed operations, handles and copied data. They never receive
BCI/MMIO/VGA mappings, PCI controls, scanout mappings or device addresses.
The kernel produces every packet and target/texture base. Validate ownership,
generation, state enums, dimensions, pitch, full byte extents, vertex/index
counts, index ranges and arithmetic overflow before queueing. Set scissors
from the intersection of target bounds and the authorized window/lease.
Address validation must cover padding, all mip levels and compressed blocks,
not only the first texel. Retain resource references until completion.
DRM's state/address checks and copied submission buffers establish the need
for validation; Ciuki uses the stricter typed interface.
[L-S](https://sources.debian.org/src/linux/6.1.4-1/drivers/gpu/drm/savage/savage_state.c).

Process death detaches its handles and cancels unsubmitted jobs; kernel-owned
in-flight objects remain until completion or proven shutdown. Logical failure
of one request returns an error without activating or resetting the GPU.
Hardware timeout, invalid/dead status or unexplained fence progress quarantines
the common engine, cancels pending jobs with `-EIO`, and retains all potentially
referenced allocations and leases until reboot or separately proven shutdown.
Do not automatically retry a partly executed frame or overlapping blit.

Software fallback after a hardware failure requires a new frame reconstructed
from RAM **and proven CPU-safe display access**. If safety cannot be proved,
keep graphics writes stopped and retain serial/text-history diagnostics.
Quarantine is permanent for that boot; no failure-driven reset loop.

## 5. T23 memory plan

All figures below use binary MiB/KiB and are an **allocation proposal for a
confirmed 16 MiB device**, not measured free VRAM. f7-02 must log firmware
VRAM, active scanout extent, cursor/firmware reservations, BAR alias extents,
free/largest VRAM block and available/reserved host RAM before allocating.
No system memory cap is changed. Usable VRAM is the intersection of verified
capacity and registry-safe ranges, with every existing reservation subtracted.

The initial path keeps the existing linear scanout and renders into tiled
offscreen targets. Two offscreen color buffers permit bounded presentation
and reuse without rendering into the copy source. Use one D16 buffer, with
depth work serialized between frames.

| Allocation at 1024×768 | Calculation | Budget |
| --- | --- | --- |
| Existing linear XRGB8888 scanout | `4096 × 768` | 3 MiB |
| Two tiled XRGB8888 render/present buffers | `2 × 4096 × 768` | 6 MiB |
| D16 depth | `2048 × 768` | 1.5 MiB |
| Resident texture heap | Including tile/mip padding | 4 MiB |
| Cursor | Existing reserved page | 4 KiB |
| Alignment/firmware safety reserve | Budget, not a claim of firmware use | 508 KiB |
| **Total** | | **15 MiB** |
| **Uncommitted headroom** | Before any additional discovered reservations | **1 MiB** |

These pitches fit the proposed scene geometry; use actual padded tile pitches
and heights when deriving extents on hardware. If an existing firmware pitch
or reservation makes the set exceed capacity, reduce the texture heap or
reject hardware creation; do not silently alter 1024×768 or the gate's color
format. A later qualified page-flip path may remove the extra linear front
buffer, freeing 3 MiB. RGB565 and D24S8 configurations require separately
reported budgets; D24S8 consumes 3 MiB at this size, not 2.25 MiB.

Targets/depth/textures use a checked allocator with at least 4 KiB base
alignment initially, satisfying the 2 KiB target/depth base granularity in
S3-R while leaving room for generation-specific stronger constraints. Keep
tile padding and guards within each allocation. Distinguish color/depth and
texture tile formats; SuperSavage's destination descriptor selector is not
Savage4's 16/32-bit selector. [S3-R](https://vgamuseum.info/images/doc/s3/savage4_registers.pdf#page=207),
[X-A](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_accel.c).

Textures retain bounded RAM shadows. The 4 MiB VRAM heap uses fence-aware
eviction; an in-flight or bound texture cannot be overwritten. Upload/update
may wait boundedly, allocate a new version, or return `-EAGAIN`/`-ENOMEM`.
No user upload writes directly into an active GPU allocation. The native
default maximum is **1024×1024**, subject to the entire padded mip chain fitting
the queried budget; 2048 is only a later optional queried limit. A complete
1024² RGB565 mip chain is about 2.67 MiB; RGBA8888 is about 5.33 MiB and does
not fit this 4 MiB heap. No texture-size query promises successful allocation.

The fallback at 1024×768 needs, at minimum, two XRGB RAM color buffers (6 MiB)
and one D16 RAM depth buffer (1.5 MiB), plus textures/staging. Allocate lazily,
charge every shadow and avoid a second copy of an existing desktop surface.
Allocation failures return errors or a user-requested lower-resolution mode;
they never change the reported hardware benchmark resolution.

## 6. Double buffering and the presentation lease

DRM's `savage_dispatch_swap` copies back to front using the 2D engine; a
historical DRI swap is therefore not proof of a page flip. RE046-B has stream
base/select controls with VSYNC behavior, but the T23's generation must be
checked against `SavageSetGBD_PM`/`SavageAdjustFrame`, not programmed using
Savage4's select bits by assumption.
[L-S](https://sources.debian.org/src/linux/6.1.4-1/drivers/gpu/drm/savage/savage_state.c),
[S3-R](https://vgamuseum.info/images/doc/s3/savage4_registers.pdf#page=160),
[X-A](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_accel.c),
[X-D](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_driver.c).

**Initial presentation:** complete 3D work on an offscreen tiled color target,
then submit a qualified tiled-source-to-linear-front 2D copy through the same
engine service. Keep the scanout address/format/stride unchanged. Target
states are `free → rendering → ready → copying → free`; copying completes
before reuse. If that conversion blit cannot be qualified, a kernel-owned
detile/readback plus ordinary CPU upload is a correctness path, with its cost
and backend reported. It is not a pass for the hardware presentation gate.

Windowed 3D delivers a completed RAM surface to the existing desktop, through
bounded readback/detiling or software rendering. No client writes over desktop
chrome; no screen-global scissor supplied by the client is trusted. Windowed
readback performance is reported separately from the full-screen gate.

**Optional page flip:** f7-04 may qualify an optimized path under the same
lease. Both scanout buffers must have identical supported layout, pitch,
format, size and alignment. A tiled rendered image cannot be displayed as
linear bytes merely by changing its base. The driver must preserve/restore
firmware stream state, confirm the active IGA and actual base-address latch,
and prove completion plus a safe vblank switch. Keep the old front pinned
until scanout has left it. Keep at most one pending flip; do not reuse a
front/pending buffer or count a register store as a displayed frame. Missing
vblank/latch confirmation times out and disables flipping safely. Triple
buffering is outside the initial contract.

The native context asks the desktop for a full-screen session. The desktop
retains mode-set/input authority, suspends composition, and delegates an
unforgeable kernel present lease bound to process, target and display
generation. The lease only permits presentation; offscreen rendering never
implies scanout authority. F7 uses the f5-08 service if already integrated;
otherwise f7-04 must supply that common prerequisite, not a parallel protocol.

Revocation on Alt+Tab, Ctrl+Esc, Windows key, Task Manager, system modal dialog,
explicit release, desktop death or client death must close admission,
cancel queued presents, cancel or drain a pending flip, and wait for any
already-issued front write before returning. An old lease must never reach
scanout after synchronous revoke. Mode changes invalidate all device-bound
targets/fences and require explicit recreation; RAM texture shadows may be
reuploaded. Never grant a client raw display authority or an LFB mapping.

On ordinary client loss, restore the previous desktop mode/input and repaint
from its RAM state within **2 s**, preserving the existing **10 s** display
watchdog as last resort. The watchdog needs a genuine completed full-screen
frame notification from this path, not a disabled deadline or an enqueue
notification. An unproven engine/bus shutdown is subject to §4.3's quarantine
exception; it cannot promise a successful mode switch or safe repaint.
These extend the existing [F5 lease contract](win32-personality-contract.md).

Copy completion permits source reuse; it does **not** prove tear-free scanout.
Expose presentation mode (`cpu-copy`, `gpu-copy`, `flip`) and whether vblank
completion is available. Vsync requests must return an explicit unsupported
result or the requested library fallback if the path cannot honor them.
Never label copy-based FPS as measured LCD refresh.

## 7. User API: libciuki3d

### 7.1 Native interface and semantics

`libciuki3d` is a native immediate-mode C library, with the conventional name
prefix `ciuki3d_`. "Immediate mode" means state plus submitted geometry;
there is no retained scene graph and no claim of an OpenGL binary ABI. The
SDK headers and sysroot overlay must match. User wrappers follow the SDK's
error convention; the kernel wire service returns negative errno.

| API family | Required behavior |
| --- | --- |
| `query_caps`, `create_context`, `destroy_context` | Versioned caps; backend policy `auto`, `software`, `hardware-required`; dimensions, target/depth format, resource budget and actual backend. Hardware-required fails explicitly when unavailable. |
| `create_vertex_buffer`, `update_vertex_buffer`, `destroy_vertex_buffer` | Process-owned typed vertices with copied/snapshotted updates; no physical addresses. Indexed and non-indexed draws; index range and count checked. |
| `create_texture`, `update_texture`, `destroy_texture`, `bind_texture` | Immutable submitted versions, bounded rows/mips, explicit pixel formats and two stage slots. RAM shadows enable fallback/recreation. |
| `begin_frame`, `clear`, `set_state`, `draw_triangles`, `end_frame` | Ordered clear/color/depth, viewport/scissor, shading, textures, depth and blending; end returns a fence. Clear depth defaults to far (1). |
| `wait_fence`, `readback`, `present` | Bounded wait, canonical RAM pixels, desktop-authorized presentation. Rendering completion and presentation completion are distinct. |
| Context-loss/error query | Stable errors for unsupported state, stale/lost resource, revoked lease, queue saturation, allocation failure and quarantined device. |

The canonical input is transformed screen-space geometry: top-left origin,
pixel centers at `(x+0.5,y+0.5)`, normalized depth `[0,1]` (smaller is nearer),
positive reciprocal W, RGBA diffuse color and up to two normalized UV pairs.
Native values use explicit scalar fixed-point formats; f7-01 must freeze
their wire representation, numeric bounds and overflow proofs before code.
Use subpixel screen coordinates and widened integer intermediates. Clip
before narrow conversion; validate bounds again in the kernel. Never turn
out-of-range arithmetic into truncated device addresses or coordinates.

The library provides CPU model/view/projection, homogeneous clipping, culling
and lighting helpers. Perspective correction interpolates reciprocal W and
UV/W before recovering UV; normalized Z is interpolated according to the
target's depth convention. This deliberately replaces the demo's current
"larger reciprocal depth is nearer" internal representation. Native flat
shading uses an explicit per-triangle color, so later adapters can select
their API's provoking vertex rather than depend on a device default.

Required core state: flat/Gouraud/textured triangle lists; clockwise/
counterclockwise/no culling; depth enable, write mask and all eight standard
comparisons; source/destination blend `ZERO`, `ONE`, `SRC_ALPHA`,
`ONE_MINUS_SRC_ALPHA` with additive blend equation; nearest/bilinear sampling,
repeat/clamp-to-edge, `REPLACE`/`MODULATE` and a second modulating texture
stage. Texture alpha and vertex alpha have defined combine order. Native
alpha test uses the same comparison family with an 8-bit reference; advertise
hardware execution only for the qualified precision/threshold behavior.

Depth quantization, channel rounding and filter precision must be explicit
per queried format. Shared edges follow one coverage convention without
cracks or double writes. Sampling/coverage/fragment ordering are grounded in
[GL-1](https://registry.khronos.org/OpenGL/specs/gl/glspec11.pdf); this native
subset is not full OpenGL 1.1 conformance. f7-03 records the measured hardware
precision and reproducible reference-image tolerances before hardware gates.

### 7.2 Software backend and portability

Provide a scalar **SIMD-free fixed-point** rasterizer, compiled for the
existing i686/CMOV baseline. No MMX/SSE requirement, FPU requirement, machine
image variant or vendor-register access is added to generic VBE presentation.
Extend/refactor the existing demo's triangle behavior into a shared backend;
do not maintain incompatible depth/texture semantics per application.

Use bounded tiled/span loops, incremental edge functions, checked wide
interpolants and perspective-correct texture sampling. Host tests establish
overflow limits, near-plane clipping, degenerate triangles, shared-edge
coverage, depth ordering, blending, texture wrap/filter and two-stage output.
For unsupported optional hardware state, use the software backend for the
whole frame/context before side effects. Do not alternate CPU/GPU fragments
against incompatible tiled depth or partially updated color buffers. A
switch after a fault requires loss/recreation and RAM reconstruction.

The same program/API runs on the E500, generic VBE cards and the existing
QEMU VGA models. Software correctness is mandatory; **1024×768 at 60 fps
is not required of these paths**. Lower-resolution rendering/scaling is a
user-visible option, with actual render and display sizes reported. Future
ATI/Matrox/NVIDIA/3dfx engines implement the same kernel/backend contract only
after their own research and directives; recognizing an ID is not support.

### 7.3 Direct3D 6/7 and OpenGL 1.1 mapping

These are integration interfaces for later F5 work, not extra DLLs required
by f7-00 or a promise to accelerate every old API state. Interface shapes are
checked against [Wine's older Direct3D declarations](https://raw.githubusercontent.com/wine-mirror/wine/wine-10.0/include/d3d.h)
and [types](https://raw.githubusercontent.com/wine-mirror/wine/wine-10.0/include/d3dtypes.h);
OpenGL semantics come from [GL-1](https://registry.khronos.org/OpenGL/specs/gl/glspec11.pdf).

| Personality operation | Native translation / boundary |
| --- | --- |
| D3D6 `IDirect3DDevice3`, D3D7 `IDirect3DDevice7`; `BeginScene/EndScene`, `DrawPrimitive`, indexed/VB variants | Context/frame, ordered state and checked triangle submission. Strips/fans expand into lists initially. |
| TL/FVF `XYZRHW`, diffuse/specular and UV | CPU-validated conversion to canonical screen/depth/reciprocal-W data. Untransformed XYZ/normals use CPU transform/lighting/clipping. No T&L HAL device claim. Specular/fog beyond the native core uses personality software. |
| D3D render states and texture-stage states | Qualified depth/blend/alpha/shade/cull/sampler subset maps directly; unsupported precision/combiners use software or return the defined API error. Caps must agree. |
| DirectDraw render targets, Z surfaces, texture lock/unlock, `Flip`/lost surfaces | Native resources and versioned uploads, presentation lease and completed present. Loss/recreation maps to the existing F5 surface-loss rules. A successful render fence does not implement DirectDraw `Flip` by itself. |
| GL `glBegin/glEnd`, arrays, matrix/light/texgen state | Batch into triangle lists after CPU assembly, transform, lighting and homogeneous clipping. Convert GL's bottom-left window origin and depth range explicitly. |
| GL texture objects, `glDepthFunc`, `glBlendFunc`, `glAlphaFunc`, flat/Gouraud shading | Native textures/state where semantics and measured precision match; CPU fallback for borders, unsupported blend factors, stencil, fog, logic ops and other missing core behavior. |
| GL `glFlush/glFinish`, read pixels, buffer swap | Submission/bounded completion/readback and the authorized present path; never treat `glFlush` as a completion wait. |

D3D and GL raster origins, pixel-center offsets, depth ranges and provoking
vertices need independent fixtures. The common library must not bake in a
D3D half-pixel assumption or an unconditional GL Y inversion. OpenGL 1.1
requires much more than triangles (including points/lines, pixel operations
and state); later personality work must implement those semantics, often in
software. S3TC and multitexture are optional GL extensions, not OpenGL 1.1
core requirements. No full ICD/conformance claim is made by F7.

## 8. Performance expectations and acceptance

### 8.1 Honest throughput expectations

The GPU accelerates setup/rasterization, depth, sampling and selected blending;
the Pentium III still transforms, lights, clips and feeds vertices. Large
textures, high overdraw, blending destination reads, repeated uploads,
readback and CPU-fed small packets compete for bandwidth. Dual texture units
do not double pixels per frame. Shader-era games and hardware T&L workloads
need software work or a different backend.

Independent arithmetic: `1024 × 768 × 60 = 47,185,920` visible pixels/s.
At 32 bpp, scanout or one full color write is **188.74 MB/s**; a full GPU
back-to-front copy adds **377.49 MB/s** of VRAM read/write traffic. A full
color clear, D16 clear/read/write and one uncached 32-bit texture sample
raise a simple one-pass copy-present traffic model to about **1.416 GB/s**,
before overdraw/stalls. Caching or a real flip may reduce it. These are a
workload model, not measured engine bandwidth or minimum CPU-upload speed.

For illustration only, 143 MHz SDR with a 64/128-bit memory bus would have
1.144/2.288 GB/s theoretical transfer bandwidth; one pixel/clock at that
clock would be 143 Mpixel/s before stalls. **Neither clock, bus width nor
pixel/clock rate is established for the owner's T23 by this research.** The
IXc-166 announcement describes another SKU; do not substitute its clock.
Record the actual device/firmware and measurements before judging headroom.
A small rotating cube at 60 fps is plausible; a general textured game at
1024×768/60, especially with overdraw, is not promised.

### 8.2 Gates

| Gate | Required evidence |
| --- | --- |
| H1: host semantics/security | Fake engine models for FIFO/stuck clock/dead MMIO/global state/fence wrap; forged/stale/cross-process handles, extent/count overflow, malformed textures/indices; queue fairness/saturation; process death, lease revocation, partial completion and quarantine. All rejected requests cause no hardware write. |
| H2: renderer equivalence | Deterministic scalar reference scenes and per-generation layout/packet fixtures; flat/Gouraud, depth, perspective texture, two stages, blending, alpha, clipping and edges. ASan/UBSan where applicable. Existing Savage, desktop/display and demo suites remain green, run sequentially. |
| G1: integration | Lead builds one canonical image; static image checks, prior F0–F2 and relevant later gates stay passing. Lead runs existing QEMU profiles plus the F7 software cases via runtime fw_cfg and ordinary overlays; no build variants. |
| G2: fallback | QEMU without a Savage 3D model and the E500/generic VBE paths run the same native scene in software, with backend identity, correctness, resource cleanup, focus/mode restoration and allocation-failure evidence. QEMU does not qualify Savage registers or physical FPS. |
| T1: native T23 correctness | 5333:8c2e, revision/BAR/firmware identity, VRAM/reservations, selected status signature; qualified offscreen tiled render and GPU conversion copy; no guard corruption; mixed 2D/3D/cursor transitions; clean mode/lease restoration. 3D packet, completed-triangle/fence and GPU-present counters must increase. CPU transform work is expected; CPU rasterization is not a hardware pass. |
| T2: performance | Graphics Demo cube **and** the textured scene below, rendered at **1024×768**, XRGB8888 + D16, at least **60 completed rendered-and-presented frames/s** on the T23. Hardware backend and GPU copy or qualified flip must remain selected throughout. |
| T3: lifecycle/failure | Repeated create/destroy and at least 100 normal lease cycles without leaked resources; client exit/crash, queued-present revoke, target loss/mode change, blocked FIFO, unsupported hardware, safe mode and persistent failed-attempt coverage. Ordinary client failures restore desktop/input within 2 s. No forced live bus lock is required; unsafe-device exceptions are reported. |

T2 uses two independently measured deterministic scenes:

- Cube: twelve submitted triangles, approved 256×256 Ciuki texture, perspective
  rotation, depth test/write, flat and Gouraud variants. Projection uses the
  full 1024×768 target; neither a 320×240 render nor scaling counts.
- Textured scene: floor and rear wall plus eight rotating cubes, **100
  triangles** total (4 for floor/wall, 96 for cubes), the approved portrait
  and a deterministic checker texture, one two-stage modulated surface and
  a source-alpha blended wall overlay formed from two more triangles
  (**102 submitted triangles** including the overlay). Include overlapping
  foreground geometry to exercise depth. The test fixture must pin camera,
  coverage, texture dimensions, state and animation seed before benchmarking;
  shrinking coverage to obtain a pass is a workload change requiring review.

Preload resources; warm up 5 s, then measure three sequential 30 s runs per
scene/variant. Each run must meet 60 fps by completed present count divided
by calibrated elapsed time, with no dropped/lost/error presents. Report
median/p95/p99 render-to-present and submission latency, queued/dropped counts,
actual render size/format, present mode, clocks available, CPU cost, uploaded
bytes, memory use and the canonical **image SHA-256**. Collect aggregate
records outside per-frame hot paths; verbose serial timing is not a benchmark.
Report uncapped throughput separately from any vsync-paced run. If only copy
completion is measurable, call the result completed-copy FPS; it does not
establish 60 distinct LCD refreshes. Vblank timing evidence is required to
claim displayed/vsynced FPS. If the measured refresh is below 60 Hz, report
that separately rather than redefining throughput.

The f7-11 review-round-3 sample-space policy supersedes colour-distance
admission for textured geometry. Unknown silicon precision must pass the
per-boot perspective probe before the projective feature is advertised.

| Quantity | Qualified tolerance / gate |
| --- | --- |
| Coverage, shared edges, copy and texture addressing | Exact; missing pixels, cracks and layout errors fail |
| D16 / occlusion | +/-1 D16 numeric rounding; wrong occlusion fails |
| Projected sample position | <=1/16 texel per axis from the exact rational sample |
| Filter fractions | At least 8 bits; truncation/rounding uncertainty <=1/256; isolated edge sweep resolves >=192 levels/unit |
| Mip LOD | <=1/16 octave; both selected-level boundaries and trilinear weights enter the envelope |
| Texture colour | One common RGBA sample-position/LOD witness per stage within the declared neighbourhood, plus one final channel level; independent channel extrema alone cannot admit a colour |
| Sampled blending | Apply the blend equation to that same joint source RGBA witness against the exact destination; one final channel level |
| Sampled alpha test | A joint source witness must support the observed pass/fail decision and its complete local colour/depth outcome; mip uncertainty enters alpha too |
| Extended numeric domain | Boot-qualified textures <=256 square in either unit; shared W ratio <=4, normalized reciprocal W 64..256; REPEAT raw UV <=4096, common integer rebase to absolute UV <=16; input error <0.005 texel |
| Unknown arithmetic | Parameterized host model and boot probe; these bounds are acceptance policy, not a vendor precision claim |

See [Savage full-size admission](savage-fullsize-admission.md) for numeric
ranges, probe scenes, primary-source limits and fallback decisions.

Hardware comparison uses predeclared tolerances for coverage/interpolation
and color/depth precision; exact scalar-to-scalar images remain deterministic.
Missing pixels, cracks, wrong occlusion or incorrect texture layout are
failures, not rounding tolerance. A scene/state rendered partly in software
may establish correctness but cannot pass the hardware performance gate.
If T2 fails, record the bottleneck and keep the gate open; do not lower the
threshold, relabel fallback, enable risky DMA or silently switch to 16 bpp.

## 9. Directive sequence and estimates

Estimates are active engineering days including host tests and review fixes,
not calendar promises. Lead-controlled QEMU/hardware sessions and unforeseen
SuperSavage tiling/status behavior add time. Native groundwork can proceed
while F5 progresses; personality DLL implementation is excluded from these
six estimates. Each directive needs lead acceptance criteria before code.

| Directive | Deliverable / completion boundary | Dependencies | Estimate |
| --- | --- | --- | --- |
| **f7-01 — Native API and scalar reference** | Freeze versioned typed ABI, numeric bounds/error/caps/resource limits and loss semantics; implement libciuki3d software backend and CPU transforms/clipping; complete independent renderer/security fixtures. No GPU activation. | Cross-review of this contract | 3–5 days |
| **f7-02 — Shared Savage owner and VRAM** | Refactor common 2D/3D service without breaking 2D; ring/fences/admission, reservations, BAR2 alias mapping, attempt markers and quarantine; qualify vertex packet layout, minimal offscreen triangle and detile readback through MMIO. Lead records T23 tiling viability before broader activation. | f7-01; existing f1-27/f1-49 evidence | 4–7 days |
| **f7-03 — Fixed-function rasterization** | Native flat/Gouraud/textured triangles, D16 depth, clear, core blend/alpha and two texture stages; fenced uploads/eviction, chip-specific packing and golden fixtures. Optional DXT only if tested; defer unsupported state honestly. | f7-02 offscreen gate | 4–7 days |
| **f7-04 — Lease and presentation** | Integrate the f5-08 common delegated lease or implement that prerequisite; GPU tiled-to-linear copy, double-buffer lifetime, sync revoke, watchdog, loss and desktop restoration; optional separately qualified page flip. Windowed correctness uses completed RAM surfaces. | f7-02/03; coordinate shared protocol with F5 | 3–5 days |
| **f7-05 — Graphics Demo consumer** | Move cube/scene to libciuki3d, native-resolution and backend controls, full-screen request, actual completion metrics; preserve English UI and exact Ciuki identity. Lock T2 fixtures and adapter mapping tests; no F5 compatibility claims. | f7-01/03/04 | 2–3 days |
| **f7-06 — Qualification and bounded optimization** | Complete H/G/T gates with lead evidence, fault/resource/fairness audit and performance profiling; tune MMIO batching/copy path within proven FIFO/latency rules. Record results and unsupported cases. No DMA enablement to rescue a failed gate. | f7-05; all earlier gates | 3–5 days |
| **Total** | Native library + first hardware backend + demo | Hardware uncertainty is additional | **19–32 days** |

Before f7-02 writes a new register, its directive must close the remaining
technical questions with pinned source or bounded silicon evidence: the
SuperSavage offscreen tile layout and BAR2 descriptors, precise vertex
packing/depth mode, cache flush requirements without a COB, conversion-copy
support with linear scanout, and reliable idle status. If upstream's COB
dependency proves essential, amend and cross-review the memory/queue contract
before enabling it. Before flipping, close the active-head/latch/vblank and
firmware-restore questions. These are implementation gates, not permission
for speculative writes.

F7-00 is complete when this source-backed contract and its handoff report are
available. Lead cross-review/integration, production implementation, canonical
builds, QEMU and physical T23 performance evidence remain subsequent work.


## VMware SVGA3D family integration (f7-31, merge6)

Research and repository comparison: 2026-10-28 (`date` checked). The
[f7-31 family design](f7-family-vmware-svga3d.md) records the pinned protocol,
capability gates, command/state citations, memory plan and Q1–Q4 model evidence.
The VMware descriptor is `R3D_UNQUALIFIED`; registration alone admits no
hardware features. The existing NVIDIA software-only decision stays in force.

Linux v6.1 [`vmw_supports_3d`, lines 35–80](https://raw.githubusercontent.com/torvalds/linux/v6.1/drivers/gpu/drm/vmwgfx/vmwgfx_cmd.c)
requires `SVGA_CAP_3D` and checks legacy FIFO hardware version or GB devcaps.
Mesa 21.3.9 [`SVGA3D_FIFOReserve`, lines 107–128](https://gitlab.freedesktop.org/mesa/mesa/-/raw/mesa-21.3.9/src/gallium/drivers/svga/svga_cmd.c)
encodes a command id and payload size before the body. VirtualBox commit
`b6bea72111fcff48f31c57c9f66365b1d3301bb9`
[`DevVGA-SVGA3d-ogl.cpp`, lines 1526–1527 and 5687–5697](https://raw.githubusercontent.com/VirtualBox/virtualbox/b6bea72111fcff48f31c57c9f66365b1d3301bb9/src/VBox/Devices/Graphics/DevVGA-SVGA3d-ogl.cpp)
leaves TEXTURE_OPS empty and ignores colour/alpha combiner state. The pinned
local copies were checked for these facts; raw online fetches failed in this
sandbox. [Mesa's official SVGA3D documentation](https://docs.mesa3d.org/drivers/svga3d.html)
was accessible online and confirms that the guest driver uses the host GPU.

Decision: preserve integration 6's Savage engine, backend lifecycle hooks,
native fullscreen lease checks and TinyGL-only GL host link. Add VMware's
whole-context refusal flag alongside those hooks, and select VMware's host
engine and three H1 scenarios only behind `GL_KERNEL_FAMILY_VMWARE`.
The VMware model uses the copied presentation path; it does not claim the
Savage-specific direct-scanout or mode-reacquisition evidence. No hypervisor
qualification follows from these host tests.
