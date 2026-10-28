# f7-32 (optional, last): hardware 3D backend for S3 ViRGE / DX / GX / GX2 / MX (S3d engine)

- **Written:** 2026-10-26 17:15 by the f7-20 implementer for the lead. Implementer: Codex gpt-6.1-sol, effort xhigh (a Claude Fable agent if Codex is out of usage). Base: main be7852468 (f7-02 universal model, f7-03/f7-09/f7-10/f7-11 Savage backend merged) plus the f7-20 amendment of docs/design/f7-3d-acceleration-contract.md (sections A–G).
- **Owner decisions:**
  - 2026-10-25: the 3D contract covers all integrated GPU families with drivers.
  - 2026-10-26 16:49: close the whole 3D question; after the Savage the other families come next.
- **Status:** optional and last in the f7-20 order (amendment D.6, open question G.3). The S3d engine is 1996-class: one texture unit, 256x256 textures at most, D16, no multitexture; Quake III-class rendering is not reachable (single stage, fill rate). The value is closing the "every family with a 3D engine and sources" claim for 1998 budget PCs, with demo-class expectations stated honestly.
- **Constraint:** this family cannot be tested on real hardware here, and QEMU emulates no ViRGE 3D engine. Everything is host-tested against a fake engine, probe-gated and inert until the boot qualification passes on the real chip.
- The 2D driver is in src/kernel/drivers/video/s3virge/ (matches 5333:5631 ViRGE, 883d ViRGE/VX, 8a01 DX/GX, 8a10 GX2, 8c01 MX, 8c03 MX+; RGB565 modes only, see `format()`/`firmware()` in engine.c). Its render3d family descriptor is R3D_NOT_IMPLEMENTED (src/kernel/drivers/video/s3virge/engine.c:19, potential 1 unit / 256).
- The reference implementation to mirror is the Savage backend: src/kernel/drivers/video/s3savage/savage3d*.c, the render3d service, docs/design/f7-3d-acceleration-contract.md (f7-20 amendment, section A facets and section B rules) and docs/design/savage-fullsize-admission.md. The f7-02, f7-03, f7-09, f7-10, f7-11 and f7-13 directives are in docs/directives.

## Sources (never invent register programming)
- S3 "ViRGE Integrated 3D Accelerator" databook, August 1996: https://vgamuseum.info/images/doc/s3/virge.pdf (sha256 `9178eb4f908b709c34e7245c048c99413741ab42a8605cc03b92d4ad0318d34f`): §15.1 old/new MMIO, §15.4 S3d Engine Programming (15.4.5 3D Graphics Drawing, 15.4.5.2 3D Triangle Drawing p.15-23, 15.4.6 Z-Buffering p.15-25, 15.4.8 3D Pixel Color Generation p.15-26, 15.4.8.1 Texture Filtering p.15-27), §15.6.2 S3d Engine Command/..., §19 S3d Engine Register Descriptions (19.4 3D Registers p.19-24), Appendix B.4 S3D registers (B-18), §23 DMA registers (research only). The preliminary 86c325 databook (https://vgamuseum.info/images/doc/s3/old/86c325_virge_manual_preliminary.pdf) is a cross-check. Revision differences (DX/GX, GX2, MX) must be established from these documents or the X.Org driver before any limit is advertised.
- Mesa 6.5.3 src/mesa/drivers/dri/s3v (https://archive.mesa3d.org/older-versions/6.x/MesaLib-6.5.3.tar.gz, sha256 `4818e4bad10155dcf0507c5290aec38e21aa029a260fd272754baddf23a06caf`): `s3v_regs.h`, `s3v_macros.h`, `s3v_inithw.c`, `s3v_state.c`, `s3v_tris.c`/`s3virgetri.h`/`s3v_tritmp.h`, `s3v_tex.c`/`s3v_texmem.c`/`s3v_texstate.c`, `s3v_context.c`, `s3v_xmesa.c`. The driver is present in Mesa 6.5.1 and 7.0.3 (Debian source listings) and absent from 7.7.1: pin 6.5.3. Its DRM counterpart never reached mainline; the Mesa driver programs the engine through MMIO and the FIFO status.
- X.Org xf86-video-s3virge (`s3v_accel.c`, `regs3v.h`, `s3v_driver.c`) and the Haiku-derived `registers.h` in the 2D driver for FIFO slots, engine-idle rules and the 2D/3D register interlock.
- Download what you need only from primary locations (the Mesa archive, X.Org release tarballs, the databook URL above). Put downloads under build/downloads/f7-sources/<name>/ and record URL and SHA-256 of each one in your design document.
- Read the vendor documentation in the read-only driver pack under third_party/ if it has any S3 ViRGE material. Never copy or redistribute it.

## Family constraints
- Submission: MMIO writes of the S3d triangle registers with the FIFO-slot check the 2D driver already qualifies; no DMA.
- Colour: the 2D driver qualifies RGB565 only; the 3D target is RGB565 with a conversion copy into the XRGB8888 colour lease or admission limited to 16-bit modes through the framebuffer-format predicate and f7-12's ranking. State the decision with its source.
- Features: one texture unit, 256 textures (revision-dependent: establish per chip), D16, flat/Gouraud, perspective-correct bilinear, alpha blend and fog per the databook; everything else (second stage, mip filters beyond what the engine does, unsupported blend factors) is refused in `validate()` and the whole context falls back to software.

## Work
1. **Design first.** Write docs/design/f7-family-s3-virge.md:
   - the chips and PCI IDs covered (from the 2D driver's table) and the per-revision limits with their sources;
   - the 3D engine features: triangle setup, vertex/parameter formats, the texture unit and formats, depth, blending, alpha test, fog, maximum texture size;
   - command submission (MMIO, FIFO slots), fences and idle detection, the VRAM layout beside the 2D lease, and the state that needs saving and restoring;
   - the boot qualification phases Q1–Q4 and the precision envelope;
   - what Quake III's state set needs, and what stays in software (expected: most of it; demo-class scenes on the engine);
   - the risks.

   Cite databook section/page and file:line in the sources for every register you will use.
2. **Implement the backend** behind the existing family descriptor, with the same contract as the Savage (f7-20 amendment, section A):
   - per-state validation before emission;
   - bounded waits and a watchdog;
   - fences and claimed-heap resources with texture upload and eviction;
   - a boot qualification (minimum, full-size, precision) that runs through the same orchestration the Savage uses: the background, budgeted path of f7-13 if it is merged in main by the time you integrate, otherwise the current hook, which the lead will adapt;
   - the admission predicate and whole-context fallback;
   - capability snapshots for opengl32 and native GL.

   Generic code stays free of family names (tests/host/test_display_driver_names.py).
3. **Fake engine and tests.**
   - A host model of the engine decoding exactly the registers you write.
   - Differential tests against the scalar reference (render3d_scalar.c) over the case families the engine admits (flat, Gouraud and textured; depth functions; blend and alpha; perspective) and refusal tests for the rest (two stages, mipmaps).
   - Register and validator fuzzing.
   - Probe containment: writes only inside claimed VRAM, restore on every path.
   - Software fallback on an unqualified chip.
   - The H1 Quake III replay through this backend's fake engine, with GPU/CPU counts (expected mostly CPU).
   - Existing render3d, display, Savage, s3virge 2D, GL and kernel latency suites stay green.
4. **Rules.**
   - Run heavy suites with /home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh <command> (host-wide slots, capped). Light single tests may run directly with nice -n 19.
   - No QEMU, no image builds, no git add/commit/merge, no dev_diary edits, no new panic() paths.
   - Respect the target frame limit (-Wframe-larger-than=4096).
   - If abi.h or a public SDK header changes, add abi_layout_test rows and regenerate the API reference.

## Acceptance criteria
- Host fake-engine differential tests pass against the scalar reference for every admitted state; refused states select software for the whole context and never emit.
- Register and validator fuzzing: no write outside the whitelist or the claimed heap, no FIFO overrun, no unbounded wait.
- Probe containment proven on success, refusal, timeout and fault paths.
- No admission without a passed Q1–Q4 qualification in the current boot; marker `3d-s3virge`.
- Design document cites databook pages and file:line for every register and states the per-revision limits with sources.

## Dependencies, parallelism and estimate
- Depends on the merged Savage backend and the render3d service; benefits from f7-13 and f7-12 when merged. Independent of the other family directives; runs in parallel, but scheduled last (lead decision G.3).
- Estimate: 5–7 active days (databook study and design 2, backend 2, fake engine and tests 1–3).

Report to build/codex/f7-32-report.md: sources with SHA-256, design, files, exact test commands and results, open questions, and the hardware qualification procedure for an owner with this card. Leave changes uncommitted.
