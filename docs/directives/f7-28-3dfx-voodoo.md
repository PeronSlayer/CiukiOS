# f7-28: hardware 3D backend for 3dfx Voodoo3 and Voodoo4/5 (VSA-100); Banshee after a 2D match extension

- **Written:** 2026-10-26 17:15 by the f7-20 implementer for the lead. Implementer: Codex gpt-6.1-sol, effort xhigh (a Claude Fable agent if Codex is out of usage). Base: main be7852468 (f7-02 universal model, f7-03/f7-09/f7-10/f7-11 Savage backend merged) plus the f7-20 amendment of docs/design/f7-3d-acceleration-contract.md (sections A–G).
- **Owner decisions:**
  - 2026-10-25: the 3D contract covers all integrated GPU families with drivers.
  - 2026-10-26 16:49: close the whole 3D question; after the Savage the other families come next.
- **Constraint:** this family cannot be tested on real hardware here, and QEMU emulates no 3dfx 3D engine. Everything is host-tested against a fake engine, probe-gated and inert until the boot qualification passes on the real chip.
- The 2D driver is in src/kernel/drivers/video/tdfx/ (matches 121a:0005 Voodoo3 and 121a:0009 Voodoo4/5; Banshee 121a:0003 is **not** matched: do not widen the match here, record it as an open item for the lead, see f7-20 amendment G.1). Its render3d family descriptor is R3D_NOT_IMPLEMENTED (src/kernel/drivers/video/tdfx/driver.c:24).
- The reference implementation to mirror is the Savage backend: src/kernel/drivers/video/s3savage/savage3d*.c, the render3d service, docs/design/f7-3d-acceleration-contract.md (f7-20 amendment, section A facets and section B rules) and docs/design/savage-fullsize-admission.md. The f7-02, f7-03, f7-09, f7-10, f7-11 and f7-13 directives are in docs/directives.

## Sources (never invent register programming)
- Mesa 7.11.2 src/mesa/drivers/dri/tdfx (local: /home/peronslayer/Desktop/CiukiOS/build/downloads/f7-sources/mesa-7.11.2/Mesa-7.11.2): `tdfx_context.c:283-289` (Banshee 1 TMU, others 2; 9 levels = 256 textures, Napalm 12 levels = 2048), `tdfx_context.h:187-203` (PCI IDs, TDFX_IS_* macros), `tdfx_tris.c`, `tdfx_render.c`, `tdfx_tex.c`/`tdfx_texman.c`/`tdfx_texstate.c`, `tdfx_state.c`. This driver is a Glide3 client: it shows which Glide calls Quake-class rendering needs (`grDrawTriangle`, `grDrawVertexArrayContiguous`, `grVertexLayout`, `grTexDownloadMipMapLevel`, `grTexSource`, `grTexCombine`, `grTexColorCombineExt`/`grTexAlphaCombineExt`, `grBufferClear`, `grDepthBufferMode`, `grDepthMask`, `grColorMask`, `grStencil*` on VSA-100, `grLfbLock`/`grLfbUnlock`), not the registers.
- The register programming is in Glide3 itself: https://github.com/sezero/glide branch `glide-devel-sezero`, pin commit `2f226f0f9225ce8ee83e6a4a7042981e719d19ee` (2026-08-21): `glide3x/h3/glide3/src/{fxglide.h,fxcmd.h,gsstdef.h,fifo.c,gglide.c,gdraw.c,gtex.c,gtexdl.c,gsst.c,distate.c,glfb.c}` for Banshee/Voodoo3, `glide3x/h5/glide3/src/...` for Voodoo4/5 (`gsstdef.h` register map, `fifo.c` command FIFO, `gdraw.c` triangle setup, `gtexdl.c` texture download, `distate.c` state packing). Licence: 3dfx Glide source licence; keep the notice, never copy whole files.
- Linux v6.1 drivers/gpu/drm/tdfx is map management only (no command submission); X.Org xf86-video-tdfx (`tdfx_accel.c`, `tdfx.h`) and the Haiku-derived `registers.h` already in the 2D driver for 2D/engine-idle rules.
- Download what you need only from primary locations (GitHub at the pinned commit, X.Org release tarballs, the Mesa archive). Put downloads under build/downloads/f7-sources/<name>/ and record URL and SHA-256 of each one in your design document.
- Read the vendor documentation in the read-only driver pack under third_party/ if it has any 3dfx material. Never copy or redistribute it.

## Family constraints
- Submission: Glide uses a CMDFIFO in VRAM/AGP memory with a hardware read pointer. Research first (cite `fifo.c` and `gsstdef.h`) whether direct register writes to the 3D register space are a supported, documented path for Banshee/Voodoo3; prefer the simpler proven path, bounded by the existing idle signature. If only the CMDFIFO is viable, the FIFO lives inside the claimed VRAM heap, never in host memory (no bus mastering), and its wrap/pointer semantics are modelled in the fake engine.
- Voodoo3 renders 16-bit colour (RGB565) with a 16-bit depth buffer; the contract's XRGB8888 colour lease needs a conversion copy (research the 2D engine's format-converting blit in the X.Org/Glide sources; if none is source-backed, Voodoo3 admission is limited to 16-bit modes through the framebuffer-format predicate and f7-12's format ranking). VSA-100 renders 32-bit and D24S8.
- Texture limits: Voodoo3 256x256 maximum, VSA-100 2048; two TMUs on both; texture memory per TMU is partitioned (`grTexMinAddress`/`grTexMaxAddress`): the heap plan must model the TMU partitions.
- Validation refuses everything outside the Glide3 feature set used by Mesa (no DST_ALPHA blend without an alpha buffer, no REPEAT beyond the hardware wrap range, no >256 textures on Voodoo3).

## Work
1. **Design first.** Write docs/design/f7-family-3dfx-voodoo.md:
   - the chips and PCI IDs covered (from the 2D driver's table) and the Banshee gap;
   - the 3D engine features: triangle setup, vertex formats, texture units and formats, depth/stencil (VSA-100), blending, alpha test, fog, multitexture, maximum texture size, colour depth per chip;
   - command submission (direct register writes or VRAM CMDFIFO), fences and idle detection, the VRAM layout beside the 2D lease, and the state that needs saving and restoring;
   - the boot qualification phases Q1–Q4 and the precision envelope;
   - what Quake III's state set needs, and what stays in software (expected: full on VSA-100; Voodoo3 at 16 bpp with 256 textures);
   - the risks.

   Cite file:line in the sources for every register or packet you will use.
2. **Implement the backend** behind the existing family descriptor, with the same contract as the Savage (f7-20 amendment, section A):
   - per-state validation before emission;
   - bounded waits and a watchdog;
   - fences and claimed-heap resources with texture upload and eviction;
   - a boot qualification (minimum, full-size, precision) that runs through the same orchestration the Savage uses: the background, budgeted path of f7-13 if it is merged in main by the time you integrate, otherwise the current hook, which the lead will adapt;
   - the admission predicate and whole-context fallback;
   - capability snapshots for opengl32 and native GL.

   Generic code stays free of family names (tests/host/test_display_driver_names.py).
3. **Fake engine and tests.**
   - A host model of the engine decoding exactly the registers or FIFO packets you emit, per chip (Voodoo3 and VSA-100 variants).
   - Differential tests against the scalar reference (render3d_scalar.c) over the same case families as the Savage (flat, Gouraud and textured; depth functions; blend and alpha; two stages; mipmaps; perspective), including the 16-bit colour conversion path.
   - Packet and validator fuzzing.
   - Probe containment: writes only inside claimed VRAM, restore on every path.
   - Software fallback on an unqualified chip.
   - The H1 Quake III replay through this backend's fake engine, with GPU/CPU counts.
   - Existing render3d, display, Savage, tdfx 2D, GL and kernel latency suites stay green.
4. **Rules.**
   - Run heavy suites with /home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh <command> (host-wide slots, capped). Light single tests may run directly with nice -n 19.
   - No QEMU, no image builds, no git add/commit/merge, no dev_diary edits, no new panic() paths.
   - Respect the target frame limit (-Wframe-larger-than=4096).
   - If abi.h or a public SDK header changes, add abi_layout_test rows and regenerate the API reference.

## Acceptance criteria
- Host fake-engine differential tests pass against the scalar reference for every admitted state on both chip models; refused states select software for the whole context and never emit.
- FIFO/register and validator fuzzing: no write outside the claimed heap or the register whitelist, no FIFO wrap error, no unbounded wait.
- Probe containment proven on success, refusal, timeout and fault paths.
- No admission without a passed Q1–Q4 qualification in the current boot; marker `3d-tdfx`.
- Design document cites file:line for every register/packet, records the Glide licence notice handling, and states the Voodoo3 colour-depth decision with its source.

## Dependencies, parallelism and estimate
- Depends on the merged Savage backend and the render3d service; benefits from f7-13 and f7-12 (accelerated-format ranking) when merged. Independent of the other family directives; runs in parallel.
- Estimate: 6–9 active days (Glide source study 2, design 1, backend 2–3, fake engine and tests 2–3).

Report to build/codex/f7-28-report.md: sources with SHA-256, design, files, exact test commands and results, open questions (Banshee match, CMDFIFO versus direct writes, 16-bit conversion), and the hardware qualification procedure for an owner with this card. Leave changes uncommitted.
