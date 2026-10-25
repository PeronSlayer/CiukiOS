# F7-03 fixed-function research and host boundary

Research: 2026-10-25. The directive and the complete F7 contract, including
both lead amendments, govern this implementation. No hardware qualification
or increased target/work/queue limits follow from host fixtures.

Primary sources inspected before implementation:

* [Mesa 7.7 archive](https://archive.mesa3d.org/older-versions/7.x/7.7/MesaLib-7.7.tar.bz2),
  `savagestate.c:savageBlendFunc_s4/savageDDDepthFunc_s4`, `savagetris.c`,
  `savagetex.c:tileInfo_pro/savageUploadTexLevel/savageTexImageSize`,
  `savage_3d_reg.h` and `savage_bci.h`, from the contract's pinned local cache.
* [Mesa 7.11.2 archive](https://archive.mesa3d.org/older-versions/7.x/7.11.2/MesaLib-7.11.2.tar.bz2),
  the corresponding Savage files, for cross-checking the historical layout.
* [Linux v6.1 Savage validation](https://github.com/torvalds/linux/blob/v6.1/drivers/gpu/drm/savage/savage_state.c)
  and event ordering in `savage_bci.c`, from the pinned local cache.
* [X.Org Savage 2.3.9](https://www.x.org/releases/individual/driver/xf86-video-savage-2.3.9.tar.bz2),
  `SAVAGEDRISetupTiledSurfaceRegs` and `GetTileAperturePitch`.
* [S3 RE046-B](https://vgamuseum.info/images/doc/s3/savage4_registers.pdf),
  pipeline registers; [OpenGL 1.1](https://registry.khronos.org/OpenGL/specs/gl/glspec11.pdf),
  texture sampling/environment and fragment operations.

Internet retrieval of Mesa archives and Linux files failed with cache misses;
the complete pinned local sources were inspected instead. OpenGL, RE046-B,
and the upstream X.Org mirror were available online. Per-file hashes and
verification commands are retained in `build/codex/f7-03-report.md`.

Repository comparison and implementation decisions:

* F7-02's texture packer and its fake interpreter both used 8x2 subtiles.
  Mesa's **Pro/SuperSavage 32-bit layout uses 8x8**, four columns by two rows
  in each 32x16 tile. Savage4 uses the different 4x8 subtile. Correct both
  models and use independent literal fixtures. The existing owner only
  qualifies SuperSavage; do not enable another chip by recognition alone.
* Keep canonical ARGB8888 shadows, expanding RGBA8888, RGB565, ARGB4444,
  luminance and alpha at creation. Original base-format semantics govern
  REPLACE/DECAL/ADD. Mip extents include the shared 4/2/1 tail and all padding.
* Explicit native comparisons map separately to reversed D16 comparisons
  and unreversed alpha comparisons. Alpha rejection precedes any depth
  write; set `wrZafterAlphaTst`. Flat colors are explicit per triangle.
* Mesa enables `texBLoopEn` with a second texture and sets the mipmapped
  LOD offset to 10/32 (an empirical correction in upstream). Emit both;
  the minimum's unmipped bias 8 remains unchanged. Host sampling models the
  intended native LOD, not an unmeasured silicon LOD approximation.
* Keep the complete per-draw state image, both-engine waits/events, guards,
  restoration and one-job/one-triangle/64x64 target limits. Two-stage packets
  remain within the 32-word FIFO, including wait/event.
* Texture storage belongs to a separate part of the already claimed heap.
  Retain canonical shadows across fence-aware LRU eviction; uploads run on
  the serialized worker after prior completion. No allocation under engine
  locks. Unsupported hardware state chooses scalar execution for the complete
  submitted draw before clear/draw/copy side effects; hardware-required refuses.
  Allocation searches stop after 64 hole probes and eviction after 32 victims,
  returning EAGAIN without device writes when further work is needed.

Canonical filtering uses Q8 bilinear fractions, nearest mip rounding, and
Q8 log2 LOD for trilinear interpolation. LOD derives from the maximum absolute
one-pixel U/V finite difference, including reciprocal-W recovery. Channels,
stage results and blend sums round half upward; channels saturate to 255.
Input U/V is signed Q16.16 in [-16,16]. At 1024 dimensions, edge products
remain below 2^52. Derivative weights saturate at 2^20 (the third is their
negative sum). The amendment below replaces early product truncation with
an exact split representation for weighted UV/W; W remains in signed 64-bit
range. Tiny-area/extreme-UV/reciprocal-W fixtures exercise these bounds.

The runtime probe's <=64x64 resource, one-triangle/one-job ceiling remains
binding for GPU execution. Larger texture shadows/layouts up to 1024 exist,
but a larger texture selects scalar for the whole AUTO draw and fails
hardware-required. The claimed texture pool is at least 256 KiB, including
guards; lower modes assign the verified residual extent after the full-mode
buffer/guard/scratch proposal. No system RAM limit is changed.

Fixtures retain the original 658 independently checked cases and add 2,534
scalar/packet comparisons, plus 100,044 independent layout boundary checks
over 363 format/dimension mip chains. Comparison tolerances are fixed before
running: coverage and sentinels exact, channel differences <=2, D16 <=1.
These are host-model tolerances; T23 filtering/alpha precision, H2 silicon
equivalence, latency and performance gates remain open. ASan execution reaches
the behavior checks, but this sandbox prevents LeakSanitizer's exit scan;
leak detection stays enabled and that run is reported as blocked.

No public ABI, SDK header, panic path, DMA, presentation authority, scanout
programming, image build, QEMU evidence or diary change is authorized here.

## Lead amendment 1: selector and perspective recovery corrections

Research and repository comparison: 2026-10-26. The continuation addresses
the two P2 findings against `cede29475`.

* [S3 RE046-B, MM485A0, printed pp. 195–196](https://vgamuseum.info/images/doc/s3/savage4_registers.pdf#page=199)
  distinguishes diffuse from current in both color and alpha argument
  selectors. Stage-1 RGB/L8 REPLACE must preserve **current alpha** with
  `00870410`; A8 REPLACE must preserve **current RGB** with `00900403`.
  The old producer emitted `00850410` / `00900400`, and the old fake engine
  assigned them the intended operations instead of decoding the selectors.
  Decode arguments and operations independently, carrying diffuse and current
  separately. Use distinct textures and mixed formats in the regressions.
  Mesa 7.7 `savage_3d_reg.h:savageRegTexBlendCtrl` confirms field offsets;
  `savagetex.c:savageUpdateTex1State_s4` supplies the historical comparison.
* [OpenGL 1.1 §3.5.1, equation 3.4](https://registry.khronos.org/OpenGL/specs/gl/glspec11.pdf#page=76)
  interpolates weighted UV/W before recovering UV. [§3.8 wrap modes](https://registry.khronos.org/OpenGL/specs/gl/glspec11.pdf#page=103)
  uses the floor-based fractional part for negative REPEAT coordinates.
  Dividing each Q32 UV×reciprocal-W product by 65536 first loses small signed
  values. Preserve both the quotient and remainder of each product through
  weighting and final division. A direct full-product accumulation can
  overflow for the already admitted signed derivative weights; the split
  representation keeps those intermediates bounded in signed 64 bits.
  Test contrasting texels at small reciprocal W on both stages and axes,
  and compare arithmetic boundary cases to a host-only 128-bit rational oracle.

For admitted fragment/derivative weights, the sum of absolute weights is at
most `2^22 + 2^16`. With `|UV| <= 2^20` and reciprocal W <= `2^24`, the
weighted high-product sum is below `2^51`, the weighted low-product sum below
`2^39`, and `|denominator| <= 2^46 + 2^40`. Normalize the low-product carry
before dividing. Split the final numerator by the denominator before scaling
its remainder; that scaled remainder plus the residual low bits stays below
`2^63`. Adjust opposite-signed fractional remainders toward zero, preserving
the exact bounded rational result. Nonpositive neighbour W and +/-32 recovery
saturation retain their existing behavior. The production path needs only
64-bit integers and compiles for i686; 128-bit arithmetic is host-test-only.

The command/result table and before/after regression evidence are recorded in
`build/codex/f7-03-report.md`. Hardware qualification and existing tolerances
remain as stated above.

Host verification of the amendment: 207 distinct-texture mixed-format cases,
16 contrasting-texel signed-coordinate cases and 20,808 exact rational oracle
cases pass. The renderer suite passes 7 tests (3,415 renderings in total);
Savage family, 2D/mode/display, presenter/mode transitions and all 15 kernel
latency tests pass. Each requested regression failed before its production
correction. The ASan renderer run reaches backend behavior PASS checks, then
fails LeakSanitizer's sandbox-restricted exit scan; leak detection was kept
enabled. Full ASan/LSan qualification remains pending outside this sandbox.
