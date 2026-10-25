# 101-d DOS native video: source decisions and host validation

The display-driver-model and DOS/DPMI 101-d amendments were written before
implementation. The Windows 98 Resource Kit, read-only `build/rskt/wrkc24_3.htm`
in the lead checkout, lines 202–214, retains unsupported drawing in the DIB
engine. No private book or driver-pack content is copied. The named Windows
driver pack is not present in this worktree; the public upstream sources below
provide the register evidence. These are window-composition decisions, not
claims that a chip has no scaling hardware.

| Registered family | Native exact STRETCH in this change | Reviewed mechanism / reason |
| --- | --- | --- |
| S3 Savage, Savage4/2000, mobile and SuperSavage | No | [XAA](https://github.com/NetBSD/xsrc/blob/trunk/external/mit/xf86-video-savage/dist/src/savage_xaa.c) provides ordinary 2D BLT; [video](https://github.com/NetBSD/xsrc/blob/trunk/external/mit/xf86-video-savage/dist/src/savage_video.c) scales the secondary Streams scanout. No arbitrary compositor clip union is established. |
| Intel i740/i810/i815 | No | [i810 video](https://github.com/NetBSD/xsrc/blob/trunk/external/mit/xf86-video-intel/dist/src/legacy/i810/i810_video.c) programs a separate overlay surface/scale registers; no corresponding operation in the existing 2D transport. |
| Intel i830–i915 | No | [overlay](https://github.com/NetBSD/xsrc/blob/trunk/external/mit/xf86-video-intel/dist/src/sna/sna_video_overlay.c) uses a separate scanout plane; textured rendering requires a different engine/context. |
| ATI Mach64 / Rage Mobility | No | [Mach64 Xv](https://github.com/NetBSD/xsrc/blob/trunk/external/mit/xf86-video-mach64/dist/src/atimach64xv.c) uses the overlay/scaler, not the existing clipped 2D copy operation. |
| ATI Rage128 / Pro | No | [r128 video](https://github.com/NetBSD/xsrc/blob/trunk/external/mit/xf86-video-r128/dist/src/r128_video.c) uses OV0 scaler state; no exact composited stretch is qualified. |
| Radeon R100–R400 | No | [Radeon video](https://github.com/NetBSD/xsrc/blob/trunk/external/mit/xf86-video-ati/dist/src/radeon_video.c) distinguishes overlay and textured rendering. Neither establishes exact stretch in our MMIO 2D transport. |
| Cirrus GD5430/34/36/46 | No | [Linux v6.12 cirrusfb](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/cirrusfb.c) supplies byte-addressed BLT/expansion, no general scaled-copy sequence. |
| NVIDIA NV3, NV04–NV40 | No | [NV video](https://github.com/NetBSD/xsrc/blob/trunk/external/mit/xf86-video-nv/dist/src/nv_video.c) includes overlay and clipped scaled-image blit paths. The latter explicitly selects centre origin and bilinear filtering, with truncated 20-bit increments; it does not establish our exact edge sampling. No guessed point-sample object/phase setup is added to the current transport. |
| Matrox G200–G550 | No | [MGA video](https://github.com/NetBSD/xsrc/blob/trunk/external/mit/xf86-video-mga/dist/src/mga_video.c) has backend, textured and filtered ILOAD paths; no exact RGB nearest sequence for our drawing transport is established. |
| SiS 300/315 derivatives | No | [SiS video](https://github.com/NetBSD/xsrc/blob/trunk/external/mit/xf86-video-sis/dist/src/sis_video.c) uses generation-specific overlay units. |
| VIA UniChrome / Pro | No | [VIA overlay](https://github.com/NetBSD/xsrc/blob/trunk/external/mit/xf86-video-openchrome/dist/src/via_xv_overlay.c) uses V1/V3/HQV scaling; composition and exact RGB phase are unqualified. |
| Trident Blade / CyberBlade | No | [Trident video](https://github.com/NetBSD/xsrc/blob/trunk/external/mit/xf86-video-trident/dist/src/trident_video.c) programs backend HorStretch/VertStretch. |
| Chips HiQV | No | [Chips video](https://github.com/NetBSD/xsrc/blob/trunk/external/mit/xf86-video-chips/dist/src/ct_video.c) uses the backend scaler. |
| NeoMagic | No | [NeoMagic video](https://github.com/NetBSD/xsrc/blob/trunk/external/mit/xf86-video-neomagic/dist/src/neo_video.c) advertises overlaid images. |
| S3 Trio64 / ViRGE | No | [ViRGE Xv](https://github.com/NetBSD/xsrc/blob/trunk/external/mit/xf86-video-s3virge/dist/src/s3v_xv.c) uses the backend Streams scaler, separate from 2D composition. Trio ordinary BLT is not a stretch sequence. |
| VMware SVGA II | No | [SVGA register contract](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/vmwgfx/device_include/svga_reg.h) does not add stretch to the legacy RECT_COPY FIFO transport; modern surface stretch is a different protocol. |
| 3dfx Voodoo3 / single-chip VSA-100 | Yes, exact row replication only | [X.Org tdfx_video.c](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-tdfx/blob/278042ff4c469f24ab7bc28e517e8a78e2821ddc/src/tdfx_video.c), `TDFXScreenToScreenYUVStretchBlit`, and [tdfxdefs.h](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-tdfx/blob/278042ff4c469f24ab7bc28e517e8a78e2821ddc/src/tdfxdefs.h) establish opcode 2, SRCSIZE and rectangle launch. [Voodoo3 guide](https://bitsavers.trailing-edge.com/components/3dfx/Voodoo3_Programming_Guide_r1.4a_199906.pdf), 7.2 and 8.4, defines RGB formats, clip select and GO. Only a one-row source with unchanged width is admitted, making sampling independent of undocumented initial phase. |

Full-screen export adds no GPU register sequence: the existing provider sets
the same indexed-8/RGB565 modes, then exports its retained BAR-backed alias.
Cache flags must agree with that alias; the existing MTRR reconfiguration
chooses WC when it can prove a safe range, UC otherwise. The
[Intel SDM Vol. 3A, 253668-084US](https://cdrdv2-public.intel.com/825758/253668-sdm-vol-3a.pdf)
memory-type alias/TLB requirements govern PTE changes and store draining.
All implementation and test outcomes are recorded in
`build/codex/101-dosvid2-report.md`; host models do not qualify physical silicon.

Host evidence: the complete DOS VM assertion body passes 60,375,467 checks,
including both cache policies and all four packed modes, exact copy-back
screenshots, grant/revoke service-point races, faults and park timeouts. Default
LeakSanitizer finalization is blocked by the sandbox, so those process exits
are not green. The display model (11 tests), command tests (3), 3dfx fixtures
(3), desktop acceleration (5) and compositor (2) pass under their repository
sanitizer policies. All 12 changed kernel C units compile with production
flags; desktop and dosbox target ELFs compile/link without warnings. No full
kernel link, image build or runtime/hardware qualification was performed.

In the full-frame host provider model, direct scanout removes 75/118/150/235
provider writes per frame for modes 0101h/0103h/0111h/0114h respectively. Exact
row replication reduces the synthetic Fit/100%/125% presentation records from
273/272/310 COPYs to 172/194/156 STRETCHes; CPU upload volume is unchanged.
These are operation counts, not measured physical timings or FPS.
