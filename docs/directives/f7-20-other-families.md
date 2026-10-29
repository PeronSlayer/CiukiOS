# f7-20: 3D for the other GPU families with drivers (research and contract amendment)

- **Written:** 2026-10-26 16:49 by the lead. Implementer: Codex gpt-6.1-sol, effort xhigh. Base: main be7852468.
- **Owner decisions:**
  - 2026-10-25: "the contract must be 3D for all integrated GPU families with drivers".
  - 2026-10-26 16:49: close the whole 3D question. After the Savage is complete, the other families come next.
- **Constraints:**
  - Only the Savage can be tested on real hardware now. QEMU emulates none of these 3D engines.
  - Each backend must sit behind its own boot probe, which writes only inside claimed VRAM and is bounded and off the startup path (as f7-13 makes it for the Savage). It may admit hardware only after qualification on the real chip.
  - Never invent register programming. Base it on the primary sources: Mesa DRI drivers (Mesa 7.x/8.x classic drivers for r128, radeon r100/r200, mga, i810, i830/i915, tdfx, sis, unichrome, mach64, nouveau_vieux for NV04-NV10; s3v where it exists), the Linux DRM kernel drivers and X.Org drivers of the same era, vendor documentation in the supplied driver pack (third_party/drivers, third_party/Ultimate Windows Driver Pack; read-only, never redistributed), and the published register manuals (for example Intel 810/815/830 PRMs, ATI and Matrox docs where public).

## Scope (documentation and plan; no driver code in this directive)
1. **Inventory.** List every display family in this tree with a 2D driver (src/kernel/drivers/video/*: chipshiqv, cirrus, i740, i810, intel2d, mach64, matrox, neomagic, nv2d, radeon2d, rage128, riva128, s3legacy, s3savage, s3trio64, s3virge, sis, tdfx, trident, unichrome, vmwaresvga) and the f7-02 family descriptors. For each one give:
   - whether the silicon has a 3D engine and what it supports: triangle setup, texture units, depth formats, blending, multitexture;
   - the primary sources available, with exact files and versions;
   - documentation quality, and how common the chip was on the 1998-2004 hardware baseline (docs/design/hardware-baseline.md);
   - what it needs for Quake III-class rendering.
2. **Contract amendment** in docs/design/f7-3d-acceleration-contract.md. Cover:
   - the per-family backend model on top of render3d: validation, packet encoders, fake engine for host tests, boot probe, admission and the precision envelope;
   - which families are worth a backend and which stay software (no 3D engine, or no usable sources);
   - the order by value: prevalence × feasibility, for example Intel i810/i815/i830, ATI Rage128/Radeon 7000, nVidia TNT/TNT2/GeForce2 through nouveau_vieux sources, Matrox G200/G400, 3dfx Voodoo3/Banshee, SiS 300/315, VIA Unichrome, S3 Virge;
   - the shared work that every backend reuses;
   - the risks.
3. **Implementation directives.** One per family, with acceptance criteria (host fake-engine differential tests against the scalar reference, packet and validator fuzzing, probe containment, no admission without qualification), dependencies and estimates. Mark which ones can run in parallel.
4. Record every source with a link and the decision in the amendment. Mark open questions for the lead.

Report to build/codex/f7-20-report.md. Leave changes uncommitted.
