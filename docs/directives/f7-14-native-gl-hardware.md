# f7-14: native OpenGL programs on the hardware 3D path (Graphics Demo consumer)

- **Written:** 2026-10-26 16:49 by the lead. Implementer: Codex gpt-6.1-sol, effort xhigh. Base: main be7852468 (f7-02..f7-11 merged).
- **Owner:** close F7 completely on the Savage.
- **Gap:** native programs use the TinyGL software port in sdk/libgl (f7-01; gfxdemo records fps). Only Win32 opengl32 reaches render3d and the Savage (f7-09/f7-10/f7-11). The contract's f7-05 "Graphics Demo consumer" (native-resolution and backend controls, full-screen request, actual completion metrics) and the native half of the API are not done.

## Scope
1. **One GL for native and Win32 programs.** Give native programs the same hardware path: native libgl or libciuki3d over the render3d transport, the same capability snapshots and the same admission and whole-context fallback as opengl32. Do not keep two incompatible GL semantics. Share the backend/encoder code with sdk/win32/opengl32 where possible, through the existing private interfaces. Generic code stays free of family names.
2. **Graphics Demo** (apps/gfxdemo):
   - choose the backend (automatic/hardware/software);
   - native resolution and a full-screen request through the common present lease;
   - show and record real completion metrics: completed-copy FPS, frames on the GPU versus software, refusals;
   - keep the English UI and the exact approved Ciuki portrait as the texture (deterministic conversion only).
3. **Tests.**
   - Host replay of gfxdemo scenes through the production native GL, transport, validator and fake Savage, with GPU/CPU counts.
   - Software fallback on unqualified devices (QEMU).
   - ABI oracle rows for any public record, and the API reference.
   - Existing gfxdemo, libgl, render3d, display and desktop suites stay green.
   - Write the QEMU check (software path) and the T23 check (hardware path) the lead will run.

Other implementers work in parallel on f7-12 (pixel formats and D24/S8 in the Savage backend) and f7-13 (boot probe scheduling). Avoid editing savage3d.c, encode.h and savage3d_owner.c beyond what is unavoidable, and list any such edit.

Report to build/codex/f7-14-report.md. Leave changes uncommitted.
