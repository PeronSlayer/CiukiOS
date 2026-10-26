# f7-31: SVGA3D backend for the VMware SVGA II device (VirtualBox VMSVGA and VMware guests)

- **Written:** 2026-10-26 17:15 by the f7-20 implementer for the lead. Implementer: Codex gpt-6.1-sol, effort xhigh (a Claude Fable agent if Codex is out of usage). Base: main be7852468 (f7-02 universal model, f7-03/f7-09/f7-10/f7-11 Savage backend merged) plus the f7-20 amendment of docs/design/f7-3d-acceleration-contract.md (sections A–G).
- **Owner decisions:**
  - 2026-10-25: the 3D contract covers all integrated GPU families with drivers.
  - 2026-10-26 16:49: close the whole 3D question; after the Savage the other families come next.
  - 2026-10-24: owner-approved images become releases for QEMU/VirtualBox/VMware; this family is what those VM images see.
- **Constraint:** QEMU's `vmware-svga` reports no `SVGA_CAP_3D` and decodes only `SVGA_CMD_*` 0–30 (hw/display/vmware_vga.c), so QEMU cannot run this backend. VirtualBox's VMSVGA (`src/VBox/Devices/Graphics/DevVGA-SVGA3d*.cpp`) and VMware implement SVGA3D on the host GPU. Whether VirtualBox is an allowed test host is an open question for the lead (f7-20 amendment G.2); until answered, everything is host-tested against a fake device and stays inert.
- The 2D driver is in src/kernel/drivers/video/vmwaresvga/ (matches 15ad:0405; FIFO engine with validated ring, `engine.c`). Its render3d family descriptor is R3D_NOT_IMPLEMENTED (src/kernel/drivers/video/vmwaresvga/driver.c:12). The local `svga_reg.h` (X.Org xf86-video-vmware 8f0cf7c) already defines `SVGA_CAP_3D` (l.438), `SVGA_FIFO_3D_HWVERSION` (l.506) and `SVGA_FIFO_CAP_3D_HWVERSION_REVISED` (l.538).
- The reference implementation to mirror is the Savage backend: src/kernel/drivers/video/s3savage/savage3d*.c, the render3d service, docs/design/f7-3d-acceleration-contract.md (f7-20 amendment, section A facets and section B rules) and docs/design/savage-fullsize-admission.md. The f7-02, f7-03, f7-09, f7-10, f7-11 and f7-13 directives are in docs/directives.

## Sources (never invent command programming)
- Linux v6.1 drivers/gpu/drm/vmwgfx/device_include/: `svga_reg.h`, `svga3d_reg.h`, `svga3d_cmd.h` (SVGA_3D_CMD_* ids, `SVGA3dCmdDefineSurface`, `SVGA3dCmdSurfaceDMA`, `SVGA3dCmdDefineContext`, `SVGA3dCmdSetRenderTarget`, `SVGA3dCmdSetTextureState`, `SVGA3dCmdSetRenderState`, `SVGA3dCmdDrawPrimitives`, `SVGA3dCmdSurfaceCopy`, `SVGA3dCmdClear`), `svga3d_types.h`, `svga3d_limits.h`, `svga3d_devcaps.h` (`SVGA3D_DEVCAP_*` queried through `SVGA_REG_DEV_CAP`), `svga3d_surfacedefs.h`; `vmwgfx_fifo.c`/`vmwgfx_cmd.c` (FIFO reserve/commit, fences through `SVGA_FIFO_FENCE`), `vmwgfx_surface.c`.
- Mesa 21.3.9 src/gallium/drivers/svga: `svga_cmd.c` (command emission for every SVGA3D command above), `svga_winsys.h` ("documentation taken from the VMware SVGA DDK"), `svga_screen.c` (devcap queries), `svga_state_*.c` (fixed-function render state mapping for VGPU9), `svga_tgsi*` is **not** needed (no shaders: SVGA3D VGPU9 fixed-function state is enough for the contract's subset; cite the render-state ids used).
- VirtualBox main branch `src/VBox/Devices/Graphics/DevVGA-SVGA.cpp`, `DevVGA-SVGA-cmd.cpp`, `DevVGA-SVGA3d.cpp`, `DevVGA-SVGA3d-ogl.cpp` (what the VirtualBox host actually implements and refuses; the hardware version it reports).
- X.Org xf86-video-vmware `vmwgfx/` headers (the local `svga_reg.h` origin).
- Download what you need only from primary locations (kernel.org/GitHub torvalds mirror at a pinned tag, the Mesa archive, github.com/VirtualBox/virtualbox at a pinned commit). Put downloads under build/downloads/f7-sources/<name>/ and record URL and SHA-256 of each one in your design document.

## Family constraints
- Capability-driven: `SVGA_CAP_3D`, `SVGA_FIFO_3D_HWVERSION` (minimum hardware version to be chosen from the sources and justified) and the `SVGA3D_DEVCAP_*` answers decide units, texture sizes, formats and depth formats at boot; nothing is hard-coded per host. The design document must state what VirtualBox's implementation reports.
- Transport: the existing FIFO engine (validated ring, drain proof, `SVGA_REG_CONFIG_DONE`) carries the 3D commands; fences use `SVGA_FIFO_FENCE`/`SVGA_CMD_FENCE` mapped onto the service's software fences. No GMR/host-memory surfaces in this directive: surfaces live in device memory and are filled through `SVGA3dCmdSurfaceDMA` from a guest-memory region claimed for the purpose, or through the simplest source-backed upload path; justify the choice.
- Precision: the host GPU's arithmetic is unknown and varies; the Q3 envelope runs on every boot and advertises only what passes (the sample-space policy of f7-11 applies as to any silicon).
- Presentation: the 3D render target is an offscreen surface; the copy into the owned linear colour buffer (`SVGA3dCmdSurfaceCopy` or readback into the framebuffer) is the only path to scanout authorized by the contract (COPY never authorizes scanout).

## Work
1. **Design first.** Write docs/design/f7-family-vmware-svga3d.md:
   - the device identity and the capability/hardware-version gates;
   - the SVGA3D features used: contexts, surfaces, render targets, texture stages and formats, depth/stencil formats, render states for blending/alpha test/depth, draw primitives, clear, copy;
   - command submission over the existing FIFO, fences, the surface memory plan beside the 2D lease, and the state that needs saving and restoring (context lifetime, surface ids);
   - the boot qualification phases Q1–Q4 and the precision envelope on a host GPU;
   - what Quake III's state set needs, and what stays in software;
   - what VirtualBox's implementation supports and refuses, with file:line;
   - the risks.

   Cite file:line in the sources for every command and render-state id you will use.
2. **Implement the backend** behind the existing family descriptor, with the same contract as the Savage (f7-20 amendment, section A):
   - per-state validation before emission (devcap-driven);
   - bounded waits and a watchdog on FIFO fences;
   - fences and claimed surface resources with texture upload and eviction;
   - a boot qualification (minimum, full-size, precision) that runs through the same orchestration the Savage uses: the background, budgeted path of f7-13 if it is merged in main by the time you integrate, otherwise the current hook, which the lead will adapt;
   - the admission predicate and whole-context fallback;
   - capability snapshots for opengl32 and native GL.

   Generic code stays free of family names (tests/host/test_display_driver_names.py).
3. **Fake device and tests.**
   - A host model of the SVGA3D command protocol decoding exactly the commands you emit (surface/context tables, render states, draw, DMA, copy), with configurable devcaps and hardware version.
   - Differential tests against the scalar reference (render3d_scalar.c) over the same case families as the Savage (flat, Gouraud and textured; depth functions; blend and alpha; two stages; mipmaps; perspective).
   - Command and validator fuzzing (malformed surface ids, oversized DMA, unsupported formats).
   - Probe containment: no surface outside the claimed plan, every context and surface destroyed on every path.
   - Software fallback when `SVGA_CAP_3D` is absent (QEMU) or the devcaps refuse.
   - The H1 Quake III replay through this backend's fake device, with GPU/CPU counts.
   - Existing render3d, display, Savage, vmwaresvga 2D, GL and kernel latency suites stay green.
4. **Rules.**
   - Run heavy suites with /home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh <command> (host-wide slots, capped). Light single tests may run directly with nice -n 19.
   - No QEMU, no VirtualBox, no image builds, no git add/commit/merge, no dev_diary edits, no new panic() paths. The lead runs any VM evidence.
   - Respect the target frame limit (-Wframe-larger-than=4096).
   - If abi.h or a public SDK header changes, add abi_layout_test rows and regenerate the API reference.

## Acceptance criteria
- Host fake-device differential tests pass against the scalar reference for every admitted state; refused states select software for the whole context and never emit.
- Command and validator fuzzing: no command with an unknown id, surface or out-of-plan size reaches the FIFO; no unbounded fence wait.
- Containment proven on success, refusal, timeout and fault paths (contexts and surfaces released).
- No admission without a passed Q1–Q4 qualification in the current boot; marker `3d-vmwaresvga`; QEMU's device always selects software.
- Design document cites file:line for every command/state id and records VirtualBox's reported capabilities.

## Dependencies, parallelism and estimate
- Depends on the merged Savage backend and the render3d service; benefits from f7-13 when merged. Independent of the other family directives; runs in parallel. Lead decision G.2 (VirtualBox as test host) gates the VM evidence, not the host work.
- Estimate: 6–9 active days (protocol study and design 2, backend 2–3, fake device and tests 2–3), plus the lead's VirtualBox session if allowed.

Report to build/codex/f7-31-report.md: sources with SHA-256, design, files, exact test commands and results, open questions, and the VM qualification procedure (VirtualBox VMSVGA with 3D enabled, VMware) for the lead. Leave changes uncommitted.
