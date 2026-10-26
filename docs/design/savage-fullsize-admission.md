# Savage full-size admission (f7-11)

## Review round 5: production batch work and framebuffer census

Research rechecked on 2026-10-26 before the amendment-5 code changes:
[OpenGL 1.1](https://registry.khronos.org/OpenGL/specs/gl/glspec11.pdf),
sections 2.6/3.5 and appendix A, preserves primitive order and framebuffer
semantics across implementation batching. Quake III's upstream
[`R_DrawElements`/ordered shader passes](https://raw.githubusercontent.com/id-Software/Quake-III-Arena/master/code/renderer/tr_shade.c)
must therefore split without dropping or reordering triangles. The pinned
[Linux v6.1 Savage BCI source](https://github.com/torvalds/linux/blob/v6.1/drivers/gpu/drm/savage/savage_bci.c),
`savage_bci_wait_fifo_s4`/`savage_fake_dma_flush`, retains bounded occupancy
checks while streaming; [RE046-B](https://vgamuseum.info/images/doc/s3/savage4_registers.pdf)
MM8508/MM48C18 distinguishes FIFO capacity from geometry work. Neither source
defines Ciuki's pixel-work limit: that is the existing per-generation policy
in `savage3d.c:query_caps`, enforced by `render3d.c:validate`/`render3d_submit`.
Online Linux/Mesa/X.Org retrieval failed; the existing pinned Linux and Mesa
vertex-format routines were read locally. No silicon, precision, tiling or
watchdog policy changes are needed.

The private GL/GDI capability handoff now carries the actual qualified
`max_pixel_work`, with a private interface version bump. The journal splits
before either the vertex count or accumulated bounding-box work would exceed
the limit. Work is `ceil(maxX-minX) * ceil(maxY-minY)` per triangle, including
subpixel spans, using the kernel's measure rather than covered fragments.
Clear commands remain separate and charge zero geometry work.

The census uses the same provider framebuffer predicate before encoding draws
or clears: XRGB with D16 and no stencil attachment. Disabling stencil testing
does not make a D16/S8 framebuffer eligible. Its reference batch model uses
the same vertex/work bounds, state and immutable texture revisions, separate
clears, the 64-command journal bound and frame boundaries. Reference display
dimensions equal the drawable dimensions, so its work limit is conservatively
`4 * width * height`; a larger qualified physical mode may allow larger
batches. `candidate_batches`, `candidate_pixel_work` and `max_batch_work` count
eligible candidate commands, including candidates in frames later pinned to
software; they do not claim GPU execution or timing. Covered fragments remain
a separate raster count. Resource eviction and explicit application syncs
can cause additional production boundaries.

Quake III's upstream
[`GLW_InitDriver`](https://raw.githubusercontent.com/id-Software/Quake-III-Arena/master/code/win32/win_glimp.c)
chooses D24 for a >16-bit desktop when `r_depthbits=0`, and allows stencil
only at depth >=24. The H1 default-format replay explicitly requests XRGB32,
D24/S8 and selects Ciuki pixel-format index 9. That context cannot open on
the current Savage provider. The separate D16/S0 H1 replay is the eligible
hardware profile; a hardware trial must explicitly select `r_depthbits 16`
and `r_stencilbits 0`. Default-format software results are not evidence of
Savage acceleration.

Research checked 2026-10-26 before implementation. This implements the lead's
[f7-11 directive](../directives/f7-11-full-size-admission.md) and supersedes only
the earlier size/list limits after a second, per-boot qualification.

Primary sources and repository comparison:

- [Linux v6.1 savage_bci.c](https://github.com/torvalds/linux/blob/v6.1/drivers/gpu/drm/savage/savage_bci.c),
  `savage_bci_wait_fifo_s4`, `savage_fake_dma_flush`, event emit/wait: CPU command
  streams need occupancy-based back-pressure and a completed event. Ciuki's
  existing 32-entry, no-COB transport supplies the bounded polling primitive;
  larger lists stream through it and finish with the existing both-engine event.
- [Mesa 7.7 savagetris.c](https://chromium.googlesource.com/chromium/deps/mesa/+/3a0db227ffe90888ad760c61a63226988c974e00/src/mesa/drivers/dri/savage/savagetris.c),
  `savageChooseVertexFormat_s4`: retain XYZ/W/color/ST0/ST1 ordering and shared
  reciprocal W; independently projected texture stages still need fallback.
- [X.Org 2.3.9 archive](https://www.x.org/releases/individual/driver/xf86-video-savage-2.3.9.tar.bz2),
  `SAVAGEDRISetupTiledSurfaceRegs` and `GetTileAperturePitch`: keep SuperSavage's
  base shift 6 and fixed 8192/4096-byte color/depth alias pitches. A full-height
  mapping is an alias of the existing claimed heap, never additional VRAM.
- [S3 RE046-B](https://vgamuseum.info/images/doc/s3/savage4_registers.pdf),
  sections 1, 10, 13: register access, perspective-enabled pipeline and status.
  Generation-specific programming continues to follow the SuperSavage source.
- [OpenGL 1.1](https://registry.khronos.org/OpenGL/specs/gl/glspec11.pdf),
  sections 3.5, 3.8 and 4.1: projective interpolation precedes filtering and
  fragment operations. Nearest/mip discontinuities make a small coordinate
  error insufficient evidence for equivalence.
- [Quake III tr_shade.c](https://raw.githubusercontent.com/id-Software/Quake-III-Arena/master/code/renderer/tr_shade.c):
  indexed lists, two stages, ordered depth/blend passes motivate larger batches.

The web mirrors for Mesa, Linux and X.Org were unavailable; the named routines
were read in the lead's pinned, read-only `build/downloads/f7-sources` cache.
No upstream code is imported. The report records the file hashes.

Admission remains 64x64/one triangle until the minimum probe and the new
full-size scene both succeed for this device/display generation. The second
probe compares color and D16 against the production scalar renderer with the
existing two-channel/one-depth tolerance, checks coverage, copies and guards,
and restores state. Allocation/preflight refusal preserves minimum admission;
an engine, guard or restore fault follows the existing quarantine policy.
The durable attempt marker spans both probes.

Qualified lists contain at most 256 triangles and four display-sized bounding
boxes of pixel work. Software retains its 65536-pixel work bound. One hardware
job remains outstanding; the existing execution deadline, event proof and
watchdog are retained. These bounds are admission limits, not a measured FPS
claim. The copied draw record grows within its existing 64 KiB slot; the wire
version changes to prevent old/new layouts from being confused.

The streaming transport validates both packet count and word extent before
writing. A single 20 ms deadline and frozen-clock status-read budget cover
FIFO back-pressure and final event completion together; progress between
packets never restarts either bound. Existing 2D transfers keep their limits.

The GL relaxation is deliberately narrower than native packet sampling:
integer-pixel triangles with a power-of-two doubled area 256..32768,
reciprocal W 0.5..2, lossless fixed-point conversion, UV -1..1, textures up
to 64x64 and equal LINEAR min/mag filters. Nearest/mip discontinuities,
independent stage Q, alpha-test and blend draws with varying sampling retain
software fallback. Differential tests compare GL's rational planes, the native
scalar reference and the independent packet interpreter at the area limits
and slanted edges, including maximum-contrast texels.

The cost model uses 1024, 3000 and 4600 triangles/frame, 256 triangles/batch,
a nominal 1 GHz Pentium III, 120 cycles/uncached command word, 250 cycles/status
observation and 100 MiB/s readback. These are explicit assumptions, not T23
measurements. Current synchronous color/D16 readback occurs after every draw
job; its cost can exceed the unchanged execution deadline at large sizes.
The fake engine does not model VRAM bus latency. A positive host gate therefore
does not predict silicon qualification or useful game throughput. T23 evidence
must determine whether bounded transfer staging is needed in a later directive;
this change does not relax deadlines, watchdogs or DMA restrictions.

Qualification belongs to the existing framebuffer generation. A real mode
change closes it; there is no implicit retry of this boot-only diagnostic.
Test a smaller Quake mode in a window, or boot in the same physical mode as
the full-screen test. Reboot for another physical mode's qualification.

## Review round 1 corrections

The production syscall's attachment payload bound is now 1024x1024x4 bytes,
under the unchanged 6 MiB wire ceiling. Exact owned resource sizes, formats,
generation and presentation authority are still checked by the dispatcher.
Worker tests exercise full-size uploads, reads and completed-copy presentation
through `w32_render3d_call`, including D16 and rejected size mismatches.

The lead's review decision separates completed CPU readback from the unchanged
20 ms engine execution window. Canonical shadows receive at most 128 reads per
chunk, checked against a fresh 1 ms budget, including guard reads. Boot
qualification reads rows 0, 16, 32, ... and the final row of color, D16 and copy;
comparison/checksums use precisely those rows. All columns and guards are still
checked. Ordinary jobs read every row. No DMA or new buffer is introduced.
CPU uploads precede the engine window; they submit no BCI command.

When completion proves idle and no event remains pending, restoration gets its
own bounded 20 ms window even after a readback/work timeout. A restored CPU
timeout fails the affected context and refuses boot qualification while keeping
the working engine available. Unproven writers and integrity/restoration faults
retain quarantine. Initial failed MMIO preflight still emits no BCI command.
This follows Linux v6.1's distinction between bounded FIFO waits and completed
events (`savage_bci.c`, linked above), and retains the source-backed X.Org tile
and AGD restoration. The timeout is a Ciuki scheduling policy, not an upstream
hardware timing guarantee. Total synchronous readback cost remains material.

The earlier varying-W GL subset is superseded. Varying UV now requires constant
reciprocal W per triangle, integral Q20 planes at pixel centers, and integral
projected coordinate gradients/centers. The dyadic geometry, lossless conversion,
UV/texture/filter bounds and alpha/blend refusals remain. These conditions remove
the intermediate GL plane truncation instead of assuming independent stages
have correlated sampling errors. OpenGL 1.1 sections 3.5/3.8 and
[OpenGL 1.2.1 appendix F.2](https://registry.khronos.org/OpenGL/specs/gl/glspec121.pdf)
require independent texture coordinates and sequential application. The exact
two-dimensional checkerboard counterexample exceeds the two-level tolerance
and is refused; varying W and other nonintegral planes fall back per draw.
Host evidence covers 96 admitted cases with independently varying stage UV,
two-dimensional checkerboards, signed wrap/clamp, both windings and area limits.


## Review round 2: sample-space qualification

Primary-source review on 2026-10-26: Mesa 7.7/7.11.2 `savagetris.c`
(`savageChooseVertexFormat_s4`) emits IEEE float XYZ, reciprocal W, diffuse,
ST0 and ST1 with one shared W. `savagetex.c` selects bilinear/mip filtering
and wrap/clamp; Linux v6.1 `savage_state.c` validates vertex skip/stride, not
interpolator precision. X.Org 2.3.9 retains the documented tile apertures.
RE046-B MM4858C/MM485A0 and MM485A8 describe filtering and perspective;
they do not specify internal interpolation or filter-weight fraction bits.
OpenGL 1.1 section 3.8 defines sampling without requiring the software
renderer's eight-bit truncation. The links above identify these sources.

The declared qualification policy is **delta = 1/16 texel per axis**, at
least **8 filter-fraction bits**, and one final channel level of rounding.
These are Ciuki acceptance thresholds, not claimed Savage specifications.
Unknown arithmetic is parameterized in the fake silicon. The boot probe
checks the resulting sample envelope with independent perspective gradients
and high-contrast transitions in each texture stage. Failing that check
retains full-size affine qualification and refuses the projective feature.
Coverage/copy and guard integrity remain exact; D16 remains within one level.
The envelope splits at texel boundaries, includes extrema within each cell,
and propagates independent stage intervals through modulation.

Admitted GL geometry uses shared reciprocal W in [0.5,2], W ratio <=4,
normalized coordinates in [-4,4], textures <=64x64, and exact Q4 coverage.
The encoder rounds W to Q16 and coordinates to Q16; the worst input recovery
error is below 0.009 texel in this range. The remaining declared budget is
reserved for the unknown interpolator. Independent stage Q, alpha thresholds
with uncertain sampled alpha and unsupported state still fall back cleanly.
The earlier requirement for dyadic triangles, integral Q20 planes and
bit-identical colours is superseded by this section.


The mip policy additionally bounds LOD error by 1/16 octave (the S3 bias
register has S4.4 encoding, but this is still a qualification threshold).
Varying mipmapped GL triangles must bound the sum of reciprocal-W gradients
per pixel to 1/32 of minimum reciprocal W. The finite-neighbour quotient
versus exact derivative differs by at most log2(32/31) < 1/16 octave in
that range. The probe includes nearest-mip and trilinear scenes with distinct
mip contents; the envelope includes level-selection discontinuities.

Seven precision phases reuse existing full-size scratch: independent smooth
and checker gradients under W ratios through four in each axis, two mip
scenes, then two independent one-dimensional black/white edge sweeps. The
last phase must resolve at least 192 different channel values per unit; a
seven-bit filter can produce at most 129 on that fixture. This prevents a
coarse filter from passing merely by spending the position-error budget.
The observed levels and precision-phase/error are logged. Qualification is
empirical for this boot, chip and mode; no undocumented silicon width is
inferred from the float vertex format alone.

The H1 complete frame contains unsupported sampled-alpha thresholds. The
native provider returns a context-wide refusal before submitting that draw;
GL closes/drains and replays the pending journal, then keeps this context in
software. For the measured H1 stream the whole pending frame fits its journal,
so no frame triangle is submitted before this decision. Generic temporary
resource refusals retain the existing recovery path. Other streams that
already flushed have a coherent checkpoint and one permanent transition,
not per-frame alternation. The separately labelled H1 world/entity prefix
measures only the qualified set and does not qualify the complete game frame.

With assumed 100 MiB/s readback, one 640x480 color+D16 checkpoint plus reload
costs 35.156 ms and the 1024x768 pair costs 90 ms, before command submission.
This rules out repeated GPU/software switching for this backend. The chosen
context policy follows contract 7.2; larger engine windows or DMA are not added.

## Review round 3: real shader states and census

Research checked before implementation: RE046-B MM48584 (printed pp.188–189)
defines eight source/destination blend selectors, destination-write flushing,
and depth writes after alpha testing. Mesa 7.11.2 `savageBlendFunc_s4` maps
these selectors and sets `wrZafterAlphaTst`; its source/destination colour
selectors differ. OpenGL 1.1 sections 4.1.3–4.1.6 place alpha rejection before
depth updates and blend the sampled source against the destination. These
primary sources are linked above; the pinned Mesa source was read locally.
Quake III's `tr_shade.c` uses ordered shader stages and texture-coordinate
modifiers. Its real demo state distribution must be measured, not inferred
from the asset-free H1 replay.

The round-3 implementation propagates independent source-channel intervals
through blending against an exact destination. Alpha rejection is ambiguous
only where the sampled-alpha interval contains both passing and failing
integer values; either complete pixel outcome is accepted there. All other
coverage, colour and depth outcomes remain mandatory. Mip selection enters
the alpha interval too. The existing sample/LOD tolerances are unchanged.

The extended boot probe must qualify 128/256 textures, both independent stages,
large signed REPEAT coordinates, sampled-alpha thresholds and the supported
blend selectors before advertising the extended set. GL rebases REPEAT by a
common integer per triangle and scales shared reciprocal W before Q16 encoding;
these transformations preserve the exact projective quotient. Numeric ranges,
the input-error budget and census instructions are recorded with the final
verification results below. The whole-context fallback policy remains binding.

### Extended numeric domain and boot phases

The qualified maximum is 256x256 per unit, including 128x128 lightmaps.
Mesa's advertised hardware maximum is 2048x2048, but it provides no proof of
precision at that limit. Larger resources remain outside this qualification.
The existing VRAM inventory and claimed extent determine the texture pool;
there is no increase to a memory limit or unmeasured VRAM reservation. Boot
scratch needs 480 KiB of that existing pool for a 256-square nine-level chain,
a 128-square eight-level chain and guards. Insufficient space refuses before
engine entry. A temporary host staging allocation replaces the old 64 KiB
command-ring scratch; it is freed immediately after the copied texture upload.

GL accepts shared positive Q20 reciprocal W in [1, 2^28], ratio <=4. It scales
each triangle's maximum reciprocal W to 256 before rounding to Q16, so the
minimum is >=64. An integer common REPEAT translation reduces raw coordinates
within [-4096,4096] to [-16,16]; a triangle whose span cannot fit is refused.
CLAMP_TO_EDGE coordinates must already fit [-16,16]. No per-vertex modulo is
used: that would change interpolation across a repeat boundary.

Coordinate recovery contributes <256/65536 = 0.003907 texel; reciprocal-W
rounding contributes at most
`256 * 32 * epsilon / (1-epsilon)`, `epsilon=1/(2*65536*64)`, or 0.000977 texel.
The combined input error is <0.005 texel, leaving >0.0575 texel of the unchanged
1/16-texel policy for the interpolator/address arithmetic. The fake engine
parameterizes 24 significant interpolant bits, a signed 14-bit texel-address
integer accumulator, 12 address-fraction bits and 8 filter-fraction bits.
These are explicit model assumptions, not documented silicon guarantees.
The admitted texel positions fit [-4096.5,4096.5], inside that address domain;
the real chip must still pass the same colour envelope. Repeat addressing and
texture layout remain exact; a narrower accumulator that loses address bits
is a refusal. Nearest sampling remains limited to the prior exact constant
sampling subset. The existing mip reciprocal-W-gradient bound still applies.

There are now 23 precision phases. Phases 1–7 retain the original independent
ramps, checkers, mip scenes and filter-level sweep. Phases 8–9 exercise signed
large coordinates at reciprocal W 64..256 and 256/128 textures in both unit
orders. Phases 10–11 test sampled alpha, including mip discontinuities;
phases 12–21 test all selector kinds and the six required Quake blend pairs,
with sampled-alpha/mip state. Phases 22–23 check both signs of constant
UV=16 with CLAMP_TO_EDGE, which exposes address overflow that REPEAT can
hide. Each scene keeps the same 20 ms engine window,
chunked readback, sampled rows, full guard verification and restoration.
Only success of all phases exposes the extended texture/blend capabilities.
A restored precision mismatch keeps the full-size affine capability and the
64-square texture limit. Depth-plane translation implements the scalar
polygon offset only when every translated vertex remains in D16 range;
otherwise the context falls back. No undocumented offset register is used.

### Opt-in real-game census

Set **`CIUKI_GL_CENSUS=N` in the Win32 process environment before creating
its GL context**, where N is 1..10000 successful swaps per report. For example,
a Win32 parent can call `SetEnvironmentVariableA("CIUKI_GL_CENSUS","60")`
before launching Quake with `CreateProcessA`; explicit environment blocks must
include that value. The existing kernel32 environment API is resolved once
at context creation. Missing, zero, empty or invalid values disable the census.
There are no histogram allocations, state scans, encoder calls or log writes
on the disabled path; draw/swap hooks perform only a null-pointer branch.
Remove the value for performance measurements.

`GL_CENSUS version=1 reference=savage-qualified` summaries report draws,
coverage_fragments, refused draws, hardware_frames, persistent context_hw,
render size, other_calls and overflow counts. A draw is one post-clipping
triangle; fragments count exact top-left geometric coverage before alpha,
depth and scissor rejection, not framebuffer visibility or a bounding box.
`GL_CENSUS_CLASS` rows aggregate matching state tuples with draw/fragment
counts and the shared encoder's admission verdict or a refusal reason.
Stage tuples are `enabled,width,height,min,mag,wrapS,wrapT,env,internal`; blend tuples
are `enabled,source,destination`; alpha/depth tuples contain enable,
function and reference/write mask. Values are GL numeric enums. Fog, signed
polygon offset, Q20 reciprocal-W min/max and Q16 absolute UV maximum follow.
W/UV power-of-two ranges distinguish otherwise matching classes. Up to 256
classes are retained per interval; overflow counts remain explicit in the
summary. A nonzero overflow means the detailed histogram is incomplete.

`hardware_frames` assumes the fully qualified 1024-square Savage capability
and adequate resource residency, independently of QEMU's installed adapter.
Once an unsupported draw or non-triangle raster operation occurs, the
reference context stays in software for the rest of its lifetime. No census
record enables hardware, changes the real provider or relaxes its admission.
Because the context remains pinned after its first refusal, `hardware_frames=K`
within an N-frame interval means its first K frames qualify and all subsequent
frames require software. The first refused frame is
`frame - frames + hardware_frames + 1`, when K < N. Context teardown emits any
final interval, marks an unswapped final frame with `partial=1`, and frees all
census memory.
The complete H1 replay and real-demo census are different evidence: only the
lead's real-game run can establish its state distribution and residual refusals.

Sink comparison: `services/ciuki.c:diagnostic` and the kernel's
`w32_diagnostic_record` deliberately reject free-form informational/debug
records. The census therefore uses the existing `CIUKI_SYS_DEBUG_WRITE` sink,
as `sdk/win32/audio/mcicd.c:log_path` already does. This needs no new syscall or
relaxed Win32 diagnostic validation. Each physical line is <=240 bytes and
appears as `[user <task>] GL_CENSUS_PART c=<context> r=<record> p=<part>
end=<0|1> data=<text>`. Join consecutive data fields with matching task,
context and record, checking part numbers from zero through end=1, to recover
the logical GL_CENSUS / GL_CENSUS_CLASS / GL_CENSUS_CLEAR records above.
The host sink verifies this physical bound and reassembles the same format.
Clear admission/refusal is counted separately so a state rejected during a
clear also pins the reference context. No other diagnostic route is changed.

Additional primary-source check: [EXT_texture_compression_s3tc, issue 4 and
base-format table](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_texture_compression_s3tc.txt)
distinguishes opaque RGB DXT1 from transparent RGBA DXT1/3/5. The repository
already owns decoded canonical pixels for these images. Their admission uses
those pixels, with RGB alpha normalized to one during the copied upload;
no compressed layout or new decoder enters the driver. GDI's existing XRGB
pixel formats explicitly advertise zero alpha bits. Consequently GL destination
alpha blend factors translate to ONE/ZERO, as in the software GL, while native
ARGB render3d retains the real destination-alpha selectors. Both interfaces
are tested separately. RGB REPLACE uses the opaque uploaded alpha with DECAL
to replace RGB while preserving incoming alpha, exactly as GL requires.

Quake III's primary [`GL_SetDefaultState`](https://raw.githubusercontent.com/id-Software/Quake-III-Arena/master/code/renderer/tr_init.c)
enables scissoring; [`SetViewportAndScissor` and `RB_SetGL2D`](https://raw.githubusercontent.com/id-Software/Quake-III-Arena/master/code/renderer/tr_backend.c)
set the view/full-screen box. A box containing the entire target is an exact
no-op and is admitted, including clear operations. Partial scissor boxes still
refuse the context because the wire does not carry an arbitrary clip rectangle.
The census records the box and a distinct `scissor` refusal.

OpenGL 1.1 section 3.8 distinguishes GL_CLAMP's border filtering from edge
clamping. Non-mip LINEAR GL_CLAMP is admitted only when every projected vertex
stays between the outer texel centres, allowing 1/32 texel for the existing
coordinate envelope. Positive shared W keeps all interior samples in that
convex interval. Replacing an outlying hardware tap by the edge texel then
corresponds to a reference sample at that outer centre within 1/16 texel.
Border-dependent coordinates and mipmapped GL_CLAMP still refuse; the oracle
retains the actual border colour rather than treating GL_CLAMP as REPEAT.

The boot envelope keeps rational projected coordinates in Q24 before texel
Q16 conversion. Its reference error is below two texel-Q16 steps at size 256;
those steps are subtracted from the declared radius, not added to it. Two Q8
LOD steps are reserved for reference-logarithm and quotient recovery error.
Larger textures therefore do not inherit the old 32-square reference error.

For varying mip sampling, let G be the sum of absolute barycentric gradients
in pixel units, S the larger texture dimension, R the reciprocal-W ratio
rounded up, and U the maximum local coordinate magnitude rounded up. With
normalized W>=64, relative W recovery error <=2^-23 and UV Q16 recovery, a
conservative derivative-error bound is
`S*(33+2*U)/2^21*(R*G+1/32)` texels/pixel. Admission additionally requires this
to be <=1/256. Finite-neighbour error, recovery error and the reference's
reserved LOD steps then satisfy
`log2(32/31)+log2(257/256)+2/256 < 1/16` octave. Skinny mipmapped triangles
outside this proof refuse with `mip-gradient`, retaining the whole-context
software policy; this limit is explicit for the real-game census. Zero-area
triangles generate no samples and do not require a LOD bound.

## Review round 4: admission parity and joint sample witnesses

Rechecked the pinned Mesa 7.7/7.11.2 shared-W vertex/filter setup, Linux v6.1
skip/stride validation, X.Org 2.3.9 tile setup, S3 RE046-B MM4858C/MM48590,
and OpenGL 1.1 sections 3.8/4.1 (primary links above). No source changes the
empirical precision limits: a filtered sample is one RGBA vector, and its
alpha controls the subsequent alpha test and blend factors.

Native admission now repeats both mip proofs from the GL encoder for each
active sampler, including constant-W mip draws. A per-pixel reciprocal-W
change above 1/32, or the round-3 derivative-recovery error above 1/256
texel/pixel, refuses before submission/IO. Degenerate triangles have no LOD
samples. Bilinear draws retain their existing admission.

The host and boot oracles use channel intervals only to prune a search. An
accepted colour needs a concrete common (u,v,LOD) witness per stage for all
four channels. Filtering, texture combination, alpha test and blending use
that same sample. The alpha-fail alternative must preserve both attachments;
an alpha-pass witness must produce the observed colour and depth together.
Both mip levels derive from the same normalized UV displacement, bounded in
base-level texels; their filter fractions are then quantized explicitly to
at least eight bits, consistently for RGBA. The declared sample/filter/LOD
neighbourhood and final one-level rounding remain unchanged. A witness
chooses a common filter accumulation rule for RGBA: either one bilinear
accumulation or rounded horizontal rows followed by vertical accumulation. These are the packet model's and scalar model's
existing arithmetic choices; the vendor sources do not specify the internal
accumulation width. The choice is consistent across all four channels, and
is not independent per-channel rounding freedom. Explicit fraction
quantization includes half-channel rounding planes that arbitrary
unquantized point searches can miss. The bounded search refuses on
exhaustion; it never promotes an unresolved channel box to a colour proof.

The permanent software-only latch belongs to the HGLRC. Replacing drawable
attachments or a provider, changing capability generations, and rebinding a
DC do not clear it. A newly created HGLRC has its own admission lifetime.
