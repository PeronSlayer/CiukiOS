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
negative sum); weighted UV/W and W remain in signed 64-bit range. Recovery
splits integer quotient and remainder before multiplying by 65536, avoiding
overflow. Tiny-area/extreme-UV/reciprocal-W fixtures exercise these bounds.

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
