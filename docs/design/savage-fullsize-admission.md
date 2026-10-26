# Savage full-size admission (f7-11)

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
