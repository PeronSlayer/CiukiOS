# f7-30: hardware 3D backend for VIA Unichrome (CLE266, KM400, K8M800, PM800/CN400)

- **Written:** 2026-10-26 17:15 by the f7-20 implementer for the lead. Implementer: Codex gpt-6.1-sol, effort xhigh (a Claude Fable agent if Codex is out of usage). Base: main be7852468 (f7-02 universal model, f7-03/f7-09/f7-10/f7-11 Savage backend merged) plus the f7-20 amendment of docs/design/f7-3d-acceleration-contract.md (sections A–G).
- **Owner decisions:**
  - 2026-10-25: the 3D contract covers all integrated GPU families with drivers.
  - 2026-10-26 16:49: close the whole 3D question; after the Savage the other families come next.
- **Constraint:** this family cannot be tested on real hardware here, and QEMU emulates no VIA 3D engine. Everything is host-tested against a fake engine, probe-gated and inert until the boot qualification passes on the real chip.
- The 2D driver is in src/kernel/drivers/video/unichrome/ (matches 1106:3122 CLE266, 7205 KM400, 3108 K8M800, 3118 PM800/CN400). Its render3d family descriptor is R3D_NOT_IMPLEMENTED (src/kernel/drivers/video/unichrome/driver.c:19). Mesa's chip enum (`via_context.h:48-51`: CLE266, KM400, K8M800, PM800) covers all four; the design document must state which Mesa chip class each matched ID maps to and cite it.
- The reference implementation to mirror is the Savage backend: src/kernel/drivers/video/s3savage/savage3d*.c, the render3d service, docs/design/f7-3d-acceleration-contract.md (f7-20 amendment, section A facets and section B rules) and docs/design/savage-fullsize-admission.md. The f7-02, f7-03, f7-09, f7-10, f7-11 and f7-13 directives are in docs/directives.

## Sources (never invent register programming)
- Mesa 7.11.2 src/mesa/drivers/dri/unichrome (local: /home/peronslayer/Desktop/CiukiOS/build/downloads/f7-sources/mesa-7.11.2/Mesa-7.11.2): `via_3d_reg.h` (1652 lines: `HC_ParaType_CmdVdata` l.45, `HC_SubA_HEnable` 0x0000 l.236, `HC_SubA_HZWBBasL` 0x0010 l.262, `HC_SubA_HZWTMD` 0x0015 l.267, `HC_SubA_HABLCsat` 0x0034 l.387, `HC_SubA_HABLRCa` 0x0038 l.391, `HC_SubA_HDBBasL` 0x0040 l.611, `HC_SubA_HClipTB` 0x0070 l.137, `HC_SubA_HSPXYOS` 0x007c l.149, `HC_SubA_HTXnL0OS` 0x0077 l.739, `HC_SubA_HTXnTB` 0x0078 l.740 and the texture-blend subaddresses that follow), `via_context.c:557-563` (two units, 10/11 levels), `via_state.c` (state emission, `OUT_RING` of sub-address words l.108-149, l.500), `via_tris.c` (vertex emission), `via_tex.c`/`via_texcombine.c`, `via_ioctl.c:676-728` (DRM_VIA_CMDBUFFER and DRM_VIA_PCICMD), `:802` (`viaFlushDmaLocked`), `via_ioctl.c:757-775` (clear/clip header words).
- Linux v6.1 drivers/gpu/drm/via: `via_dri1.c` (the command verifier state machine and register whitelist, the PCI command path that writes the verified stream through MMIO, the AGP regulator path) and `via_3d_reg.h`; v6.1 is the last mainline tree with it (removed in 6.3). The verifier's whitelist is the model for the validator. libdrm 2.4.15 `shared-core/via_3d_reg.h`/`via_drm.h` (sha256 of the tarball `6aa54bacb581f3d4fda6d9a0153446a61f238f5b57b3f9714f1346916bdb373d`) as a cross-check.
- Linux v6.12 `drivers/video/fbdev/via/` (already the 2D driver's donor, src/kernel/drivers/video/unichrome/import.json) and X.Org xf86-video-openchrome for engine-idle and 2D/3D interlock rules.
- Download what you need only from primary locations (kernel.org/GitHub torvalds mirror at a pinned tag, X.Org/openchrome release tarballs, the Mesa archive). Put downloads under build/downloads/f7-sources/<name>/ and record URL and SHA-256 of each one in your design document.
- Read the vendor documentation in the read-only driver pack under third_party/ if it has any VIA Unichrome material. Never copy or redistribute it.

## Family constraints
- Submission: the PCI command path (the verified `HC_*` sub-address stream written through MMIO, as `DRM_VIA_PCICMD` and `via_dri1.c`'s PCI dispatch do), never the AGP regulator or a host-memory command buffer (no bus mastering). Every stream the backend emits must pass an in-kernel port of the verifier's rules before any write.
- Depth: D16 first; D24S8/D32 only if the fake engine and probe cover them.
- Textures: 512 default maximum (1024 with the `excess_mipmap` semantics, which the design document must evaluate against the engine's real limits before advertising), two units; the `HTXnL0OS`/`HTXnTB` level addressing is the layout facet.
- The integrated VRAM is shared system memory: the heap plan uses the 2D driver's measured VRAM and reservations.

## Work
1. **Design first.** Write docs/design/f7-family-via-unichrome.md:
   - the chips and PCI IDs covered (from the 2D driver's table) and the Mesa chip classes;
   - the 3D engine features: triangle setup, vertex formats, texture units and formats, depth/stencil, blending, alpha test, fog, multitexture, maximum texture size;
   - command submission (the sub-address stream and its verifier, PCI path), fences and idle detection, the VRAM layout beside the 2D lease, and the state that needs saving and restoring;
   - the boot qualification phases Q1–Q4 and the precision envelope;
   - what Quake III's state set needs, and what stays in software;
   - the risks.

   Cite file:line in the sources for every register or packet you will use.
2. **Implement the backend** behind the existing family descriptor, with the same contract as the Savage (f7-20 amendment, section A):
   - per-state validation before emission (the verifier rules);
   - bounded waits and a watchdog;
   - fences and claimed-heap resources with texture upload and eviction;
   - a boot qualification (minimum, full-size, precision) that runs through the same orchestration the Savage uses: the background, budgeted path of f7-13 if it is merged in main by the time you integrate, otherwise the current hook, which the lead will adapt;
   - the admission predicate and whole-context fallback;
   - capability snapshots for opengl32 and native GL.

   Generic code stays free of family names (tests/host/test_display_driver_names.py).
3. **Fake engine and tests.**
   - A host model of the engine decoding exactly the sub-address stream you emit, with the verifier's state machine.
   - Differential tests against the scalar reference (render3d_scalar.c) over the same case families as the Savage (flat, Gouraud and textured; depth functions; blend and alpha; two stages; mipmaps; perspective).
   - Stream and validator fuzzing (every rejected stream of the verifier must be rejected before emission).
   - Probe containment: writes only inside claimed VRAM, restore on every path.
   - Software fallback on an unqualified chip.
   - The H1 Quake III replay through this backend's fake engine, with GPU/CPU counts.
   - Existing render3d, display, Savage, unichrome 2D, GL and kernel latency suites stay green.
4. **Rules.**
   - Run heavy suites with /home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh <command> (host-wide slots, capped). Light single tests may run directly with nice -n 19.
   - No QEMU, no image builds, no git add/commit/merge, no dev_diary edits, no new panic() paths.
   - Respect the target frame limit (-Wframe-larger-than=4096).
   - If abi.h or a public SDK header changes, add abi_layout_test rows and regenerate the API reference.

## Acceptance criteria
- Host fake-engine differential tests pass against the scalar reference for every admitted state; refused states select software for the whole context and never emit.
- Stream and validator fuzzing: no sub-address outside the verifier whitelist, no address outside the claimed heap, no unbounded wait.
- Probe containment proven on success, refusal, timeout and fault paths.
- No admission without a passed Q1–Q4 qualification in the current boot; marker `3d-unichrome`.
- Design document cites file:line for every register and for each verifier rule ported.

## Dependencies, parallelism and estimate
- Depends on the merged Savage backend and the render3d service; benefits from f7-13 when merged. Independent of the other family directives; runs in parallel.
- Estimate: 6–8 active days (verifier study and design 2, backend 2–3, fake engine and tests 2–3).

Report to build/codex/f7-30-report.md: sources with SHA-256, design, files, exact test commands and results, open questions, and the hardware qualification procedure for an owner with this board. Leave changes uncommitted.
