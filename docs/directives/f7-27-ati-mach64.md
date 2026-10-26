# f7-27: hardware 3D backend for ATI Rage Pro / Rage LT Pro / Rage Mobility (mach64 setup engine)

- **Written:** 2026-10-26 17:15 by the f7-20 implementer for the lead. Implementer: Codex gpt-6.1-sol, effort xhigh (a Claude Fable agent if Codex is out of usage). Base: main be7852468 (f7-02 universal model, f7-03/f7-09/f7-10/f7-11 Savage backend merged) plus the f7-20 amendment of docs/design/f7-3d-acceleration-contract.md (sections A–G).
- **Owner decisions:**
  - 2026-10-25: the 3D contract covers all integrated GPU families with drivers.
  - 2026-10-26 16:49: close the whole 3D question; after the Savage the other families come next.
- **Why first among the new families:** the owner's Armada E500 is ATI `1002:4C4D` (Rage Mobility P/M AGP), matched by this 2D driver; this is the only family besides the Savage that can be qualified on owner hardware now. QEMU emulates no mach64 3D engine. Everything is host-tested against a fake engine, probe-gated and inert until the boot qualification passes on the real chip.
- The 2D driver is in src/kernel/drivers/video/mach64/ (matches 1002:4742/4744/4749/4750/4751 Rage Pro, 4c4d/4c4e/4c52/4c53 Rage Mobility). Its render3d family descriptor is R3D_NOT_IMPLEMENTED (src/kernel/drivers/video/mach64/driver.c:21; struct render3d_family in src/kernel/drivers/video/render3d.h).
- The reference implementation to mirror is the Savage backend: src/kernel/drivers/video/s3savage/savage3d*.c, the render3d service, docs/design/f7-3d-acceleration-contract.md (f7-20 amendment, section A facets and section B rules) and docs/design/savage-fullsize-admission.md. The f7-02, f7-03, f7-09, f7-10, f7-11 and f7-13 directives are in docs/directives.

## Sources (never invent register programming)
- Mesa 7.11.2 src/mesa/drivers/dri/mach64 (local: /home/peronslayer/Desktop/CiukiOS/build/downloads/f7-sources/mesa-7.11.2/Mesa-7.11.2): `mach64_reg.h` (SETUP_CNTL 0x0304 l.283, SCALE_3D_CNTL 0x05fc l.222, TEX_CNTL 0x0774 l.305, TEX_0_OFF 0x05c0 l.304, Z_CNTL 0x054c l.374, Z_OFF_PITCH 0x0548 l.387, DST_OFF_PITCH 0x0500 l.193, ALPHA_TST_CNTL 0x0550 l.72, VERTEX_1_X_Y 0x0258 l.351, FIFO_STAT 0x0710 l.198, GUI_STAT 0x0738 l.206, DWMREG0/1 bank select l.42-58), `mach64_state.c:911` (`mach64EmitHwStateLocked`), `mach64_tris.c:422` (`mach64_draw_triangle`), `mach64_texstate.c`, `mach64_texmem.c`, `mach64_context.c:188-195` (two exposed stages, one uploaded mip level, `driCalculateMaxTextureLevels`), `mach64_ioctl.c:99-145` (DRM_MACH64_VERTEX path, buffer discard rules).
- The DRM mach64 module is not in mainline Linux. Pinned copies: FreeBSD `release/8.4.0` `sys/dev/drm/mach64_dma.c`, `mach64_state.c`, `mach64_drv.h`, `mach64_drm.h`, `mach64_irq.c` (https://cgit.freebsd.org/src/tree/sys/dev/drm?h=release/8.4.0); the freedesktop drm git history before the 2009-11-17 commit "Drop shared-core, bsd-core, linux-core and scripts subdirs" (`shared-core/mach64_*`). Use the DRM's vertex-buffer verification (`mach64_state.c`, register whitelist) as the model for the validator; use its register-write ordering, not its bus-master DMA.
- X.Org xf86-video-mach64 (`atimach64accel.c`, `atimach64io.h`, `atiregs.h`) for FIFO/engine-idle rules and the memory-map bank layout already used by the 2D driver; the Haiku-derived `registers.h` in the 2D driver for the 2D register offsets.
- DRI wiki https://wiki.freedesktop.org/dri/ATIMach64/ : only chips with a triangle setup engine (Rage Pro, LT Pro, XL, XC, Mobility); VT/Rage II/LT excluded (not in the 2D table anyway). Utah-GLX notes "DMA code needs fixing": do not use DMA.
- Download what you need only from primary locations (kernel.org/GitHub torvalds mirror at a pinned tag, X.Org release tarballs, the Mesa archive, cgit.freebsd.org at a release tag). Put downloads under build/downloads/f7-sources/<name>/ and record URL and SHA-256 of each one in your design document.
- Read the vendor documentation in the read-only driver pack under third_party/ if it has any ATI Rage Pro material. Never copy or redistribute it.

## Family constraints
- CPU MMIO register writes through the existing claimed register window only (no bus-master BM_* DMA, no AGP). Use the FIFO_STAT/GUI_STAT idle rules the 2D driver already qualified; every wait is bounded with the existing deadline and dead-MMIO detection.
- Mesa exposes two texture stages but only one uploaded mip level per texture and 1024 maximum size; do not advertise mip filters or trilinear beyond what the engine does; refuse them in `validate()` so the whole context falls back to software (no partial passes).
- D16 only; depth compare and write mask per Z_CNTL; verify the depth representation (direction and rounding) in the fake engine and the minimum probe before claiming LESS/LEQUAL.
- Rage Mobility variants: the 2D driver's chip-id check (`CONFIG_CHIP_ID`) stays the gate; qualification is per detected device, and the E500's AGP-on-mobile configuration must be recorded (aperture, VRAM size, 2D lease) in the design document before the heap plan.

## Work
1. **Design first.** Write docs/design/f7-family-ati-mach64.md:
   - the chips and PCI IDs covered (from the 2D driver's table) and the E500 identity;
   - the 3D engine features: triangle setup, vertex formats, texture units and formats, depth, blending, alpha test, fog, two-stage semantics, maximum texture size;
   - command submission (MMIO register writes, FIFO depth and idle detection), the VRAM layout beside the 2D lease, and the state that needs saving and restoring;
   - the boot qualification phases Q1–Q4 and the precision envelope;
   - what Quake III's state set needs, and what stays in software (expected: two-pass lightmaps, no mip chains, 1024 textures);
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
   - A host model of the engine decoding exactly the registers you write (bank select, setup registers, vertex registers, texture and Z control).
   - Differential tests against the scalar reference (render3d_scalar.c) over the same case families as the Savage (flat, Gouraud and textured; depth functions; blend and alpha; two stages; perspective; the mip cases must show the refusal path).
   - Packet and validator fuzzing.
   - Probe containment: writes only inside claimed VRAM, restore on every path.
   - Software fallback on an unqualified chip.
   - The H1 Quake III replay through this backend's fake engine, with GPU/CPU counts.
   - Existing render3d, display, Savage, mach64 2D, GL and kernel latency suites stay green.
4. **Rules.**
   - Run heavy suites with /home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh <command> (host-wide slots, capped). Light single tests may run directly with nice -n 19.
   - No QEMU, no image builds, no git add/commit/merge, no dev_diary edits, no new panic() paths.
   - Respect the target frame limit (-Wframe-larger-than=4096).
   - If abi.h or a public SDK header changes, add abi_layout_test rows and regenerate the API reference.

## Acceptance criteria
- Host fake-engine differential tests pass against the scalar reference for every admitted state; refused states (mip filters, >1024 textures, unsupported blend factors) select software for the whole context and never emit.
- Register and validator fuzzing: no write outside the whitelist, no out-of-heap address, no unbounded wait.
- Probe containment proven by the fake engine's guard and restore checks on success, refusal, timeout and fault paths.
- No admission without a passed Q1–Q4 qualification in the current boot; the attempt marker `3d-mach64` keeps its meaning.
- The design document cites file:line for every register; the report gives the E500 qualification procedure (BOOT.CFG, serial capture, expected `[3d]` records) for the lead.

## Dependencies, parallelism and estimate
- Depends on the merged Savage backend and the render3d service; benefits from f7-13 (background qualification) when merged. Independent of f7-21..f7-26 and f7-28..f7-32; runs in parallel.
- Estimate: 5–8 active days (design 1–2, backend 2–3, fake engine and tests 2–3), plus the lead's E500 session.

Report to build/codex/f7-27-report.md: sources with SHA-256, design, files, exact test commands and results, open questions, and the hardware qualification procedure for the E500. Leave changes uncommitted.
