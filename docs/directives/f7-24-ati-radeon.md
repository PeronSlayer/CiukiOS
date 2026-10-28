# f7-24: hardware 3D backend for ATI Radeon R100 (7000/7200/7500, Mobility M6/M7) and R200 (8500/9000/9100/9200, Mobility M9)

- **Written:** 2026-10-26 16:57 by the lead. Implementer: Claude Fable agent. Base: main be7852468 (f7-02 universal model, f7-03/f7-09/f7-10/f7-11 Savage backend merged).
- **Owner decisions:**
  - 2026-10-25: the 3D contract covers all integrated GPU families with drivers.
  - 2026-10-26 16:57: close F7 entirely and have everything ready for the next image.
- **Constraint:** this family cannot be tested on real hardware here, and QEMU emulates none of these 3D engines. Everything is host-tested against a fake engine, probe-gated and inert until the boot qualification passes on the real chip.
- The 2D driver is in src/kernel/drivers/video/radeon2d/. Its render3d family descriptor is currently R3D_NOT_IMPLEMENTED (see src/kernel/drivers/video/render3d.h, struct render3d_family).
- The reference implementation to mirror is the Savage backend: src/kernel/drivers/video/s3savage/savage3d*.c, the render3d service, docs/design/f7-3d-acceleration-contract.md and docs/design/savage-fullsize-admission.md. The f7-02, f7-03, f7-09, f7-10 and f7-11 directives are in docs/directives.

## Sources (never invent register programming)
- Mesa 7.11.2 src/mesa/drivers/dri/radeon and r200 (local: /home/peronslayer/Desktop/CiukiOS/build/downloads/f7-sources/mesa-7.11.2/Mesa-7.11.2). Linux 6.1 drivers/gpu/drm/radeon (r100.c, r200.c, CP/ring and microcode notes). X.Org xf86-video-ati. AMD's published R100/R200 register references if available.
- Download what you need only from primary locations: kernel.org git or the GitHub torvalds mirror at a pinned tag, the freedesktop/X.Org release tarballs, the Mesa archive.
- Put downloads under build/downloads/f7-sources/<name>/, and record the URL and SHA-256 of each one in your design document.
- Also read the vendor documentation in the read-only driver pack under third_party/ if it has any. Never copy or redistribute it.

## Work
1. **Design first.** Write docs/design/f7-family-ati-radeon.md:
   - the chips and PCI IDs covered (from the 2D driver's table);
   - the 3D engine features: triangle setup, vertex formats, texture units and formats, depth/stencil, blending, alpha test, fog, multitexture, maximum texture size;
   - command submission (MMIO FIFO or ring), fences and idle detection, the VRAM layout beside the 2D lease, and the state that needs saving and restoring;
   - the boot qualification phases and the precision envelope;
   - what Quake III's state set needs, and what stays in software;
   - the risks.

   Cite file:line in the sources for every register or packet you will use.
2. **Implement the backend** behind the existing family descriptor, with the same contract as the Savage:
   - per-state validation before emission;
   - bounded waits and a watchdog;
   - fences and claimed-heap resources with texture upload and eviction;
   - a boot qualification (minimum, full-size, precision) that runs through the same orchestration the Savage uses: the background, budgeted path of f7-13 if it is merged in main by the time you integrate, otherwise the current hook, which the lead will adapt;
   - the admission predicate and whole-context fallback;
   - capability snapshots for opengl32 and native GL.

   Generic code stays free of family names (tests/host/test_display_driver_names.py).
3. **Fake engine and tests.**
   - A host model of the engine decoding exactly the packets or registers you emit.
   - Differential tests against the scalar reference (render3d_scalar.c) over the same case families as the Savage (flat, Gouraud and textured; depth functions; blend and alpha; two stages; mipmaps; perspective).
   - Packet and validator fuzzing.
   - Probe containment: writes only inside claimed VRAM, restore on every path.
   - Software fallback on an unqualified chip.
   - The H1 Quake III replay through this backend's fake engine, with GPU/CPU counts.
   - Existing render3d, display, Savage, GL and kernel latency suites stay green.
4. **Rules.**
   - Run heavy suites with /home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh <command> (host-wide slots, capped). Light single tests may run directly with nice -n 19.
   - No QEMU, no image builds, no git add/commit/merge, no dev_diary edits, no new panic() paths.
   - Respect the target frame limit (-Wframe-larger-than=4096).
   - If abi.h or a public SDK header changes, add abi_layout_test rows and regenerate the API reference.

Report to build/codex/f7-24-report.md: sources, design, files, exact test commands and results, open questions, and the hardware qualification procedure for an owner with this card. Leave changes uncommitted.
