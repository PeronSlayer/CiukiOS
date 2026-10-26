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
