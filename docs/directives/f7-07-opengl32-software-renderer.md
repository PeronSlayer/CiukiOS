# f7-07 — `opengl32.dll` for Win32 programs on the software renderer: OpenGL 1.1 + the extensions the 1998–2000 games use, `wgl`, pixel formats, presentation through DirectDraw/GDI

- **Owner:** Codex, `gpt-6.1-sol`, effort `xhigh`, `--search` allowed (Khronos GL 1.1 specification, Microsoft `wgl`/pixel-format documentation, Wine's `opengl32` declarations for ABI shapes only)
- **Written:** 2026-10-20 by the lead (owner decision 2026-10-20: the 3D of the 1.0 Beta is OpenGL; Direct3D through wined3d after the beta; dev diary 2026-10-20-08)
- **Worktree:** `../CiukiOS-wt-f7-07`, branch `wt/f7-07`
- **Contract:** `docs/design/f7-3d-acceleration-contract.md` §7.2 (software backend, SIMD-free fixed point, same program on every card) and §7.3 (OpenGL 1.1 mapping); f7-01 delivered the TinyGL port in `sdk/libgl` (`port.json`, `surface.c` XRGB adapter) and the native Graphics Demo on it.

## Required

1. **`opengl32.dll`** built by the CiukiOS Win32 toolchain (`scripts/win32/builtins.py` store, like `gdi32`/`user32`), exporting the OpenGL 1.1 entry points (`gl*` of GL 1.1 complete: immediate mode, vertex arrays, display lists, matrices, lighting, texturing with mipmaps, blending, depth/alpha/stencil-less subset declared honestly in `glGetString`/caps, `glReadPixels`/`glDrawPixels`, `glGetError`), `wglCreateContext`/`wglDeleteContext`/`wglMakeCurrent`/`wglGetCurrentContext`/`wglGetCurrentDC`/`wglGetProcAddress`/`wglShareLists`/`wglUseFontBitmaps` (bitmap fonts through the font bridge) and the GDI side `ChoosePixelFormat`/`SetPixelFormat`/`GetPixelFormat`/`DescribePixelFormat`/`SwapBuffers` in `gdi32`, with the `PIXELFORMATDESCRIPTOR` layout of the public ABI. Extensions needed by the era's OpenGL games, implemented in software: `GL_ARB_multitexture` (two units), `GL_EXT_texture_env_add`, `GL_ARB_texture_env_combine` (the subset Quake III's default shaders use), `GL_EXT_compiled_vertex_array` (as no-ops with correct semantics), `GL_EXT_bgra`, `GL_EXT_texture_edge_clamp`, `GL_ARB_texture_compression`/`GL_EXT_texture_compression_s3tc` decoded on upload; `glGetString(GL_EXTENSIONS)` lists exactly what works.
2. **Renderer.** The `sdk/libgl` port (TinyGL) extended where GL 1.1 or the extensions above require it (multitexture, texture env modes, mipmaps, alpha test, fog, scissor, polygon offset, `glColorMask`/`glDepthMask`, perspective-correct texturing within the contract's bounded fixed-point rules); no SIMD, no x87 requirement beyond what the kernel allows for user processes; colour buffer XRGB/RGB565 as the display provides; 16-bit depth buffer.
3. **Presentation.** Windowed and full-screen contexts: the back buffer is a DirectDraw surface (f5-11) or a GDI DIB section (f5-10B); `SwapBuffers` presents through the existing presentation lease (f5-08) with the completed-present semantics; resolution-scaled rendering as the contract allows (render at 320×240/400×300/512×384/640×480, scaled on present, chosen per program through the shell's compatibility settings) with the actual sizes reported in the `[win32]` records.
4. **Fixture and probe.** `gl-exercise.exe` (triangle, textured quad, lit cube, multitexture, display list, bitmap text, read-back checksum) in `src/probes/win32_gl/` with the result-block protocol on a free page (`0x10040000`); kernel probe `f5:w32-gl` emitting checksums and frame counts; registration in the three tables and the loader model; suite case `w32-gl-qemu-t23` in `tests/suites/f5-core.json` authored and unrun.
5. **Tests.** Host render goldens through the Win32 host model (`tests/host/win32/gl/`): each fixture scene against stored checksums, extension string, pixel-format enumeration, context lifetime, `wglGetProcAddress` for every listed extension function; the libgl host tests of f7-01 kept green; `make CAP= apps` and `make CAP= upper` once (the store adds a DLL). No QEMU (the lead runs the probe).

## Constraints

No vendor register access, no claim of hardware acceleration; UI copy English; `rg`/`sed` only; no `systemd-run`, no image builds, no commits, no diary edits; host tests one at a time with `nice -n 19`. Report `build/codex/f7-07-report.md` with measured fixture frame times on the host as orientation only.

## Amendment 1 (lead, 2026-10-21, after two sessions; WIP commit on the branch)

Decisions: (1) x87: user processes may use scalar x87 floating point for transforms and lighting (the kernel's lazy FPU ownership covers Win32 processes; the contract's no-FPU wording binds the kernel-side presenter and the fragment backend, which stays integer and SIMD-free) — record this reading in the validation note. (2) The v3 completed-present request is accepted for integration pending the lead's review at merge; keep it minimal and versioned. (3) Scope for the 1.0 Beta: the GL 1.1 subset the era's games use must be complete and correct, the rest reports the defined error honestly. Required before delivery: user clip planes, `glTexGen` (object/eye linear and sphere map), separate 1D/2D texture state, pixel format persisted on the owning window across DC reacquisition, `glReadPixels`/`glDrawPixels` for the byte formats of the advertised extensions, display-list validation of every implemented call, shared-object deletion, `glGetString` listing only extensions that pass their tests. Deferred after the beta (documented as unsupported, error-returning): evaluators, feedback/selection, accumulation buffer, stencil. (4) The sanitized suite is rerun by the lead. Report in "Amendment 1".

## Amendment 2 (lead, 2026-10-21): merge of current `main` into this branch

`main` now contains f5-21, f5-21b and f5-07D (shared desktop fixture gate `GATE_W32_FIXTURE`, the registered observers and their result pages up to `0x100d0000`, the probe tables and known-pending lists) plus f0-49c, f1-86, f3-52 and f2-86. The lead ran `git merge main` in this worktree and left the conflict markers in: apps/desktop/build_desktop.py apps/desktop/gate.h apps/desktop/v3_fixture.c scripts/test/loader_model.py scripts/win32/builtins.py src/boot/ciukldr/menu.inc src/kernel/probes/f2_probes_desktop.c src/kernel/probes/selector.c . Resolve them so that the GL fixture uses the shared gate, `w32-gl` is registered once in each table with the result page `0x100e0000`, the suite and tests reflect the merged state, and the v3 completed-present request coexists with the merged desktop code. Then rerun the GL goldens (`tests/host/win32/gl`), `tests.host.win32.test_kernel_core`, `test_probe_registry`, `test_kernel_loader`, `tests.host.win32.user32.test_user32`, `test_desktop_gate test_runner_selectors test_boot_modes`, `make CAP= apps` and `make CAP= upper` once. Do not commit: report in "Amendment 2".

## Amendment 3 (lead, 2026-10-21, from the Codex cross-review `build/lead-scratch/f7-07-review.md` in the main checkout: verdict "request changes")

Apply every finding below with a host regression (the P1 ones first: windowed SwapBuffers PRESENT geometry, compiled-bitmap row length, fullscreen→windowed lease release, compatibility scaling of viewports, worker-thread presentation); the review text with file:line references follows.

Ranked findings:

1. **P1 — Every windowed `SwapBuffers` fails before sending `PRESENT`.**  
   [protocol.c:143](apps/desktop/protocol.c:143) requires a valid surface descriptor for the new opcode. However, [v3.c:24](apps/libdeskui/v3.c:24) supplies synthetic geometry only for `DESK_DAMAGE`; `PRESENT` reaches local validation with zero geometry and returns zero. This breaks the shipped windowed fixture despite green host rendering.
   
   **Smallest fix:** apply the existing DAMAGE geometry handling to PRESENT, and add a production `du3_request` transport regression.

2. **P1 — Compiled bitmaps can read beyond their retained allocation.**  
   [engine.c:152](sdk/win32/opengl32/engine.c:152) calculates bitmap capture pitch from bitmap width, ignoring `UNPACK_ROW_LENGTH`. Replay in [pixels.c:140](sdk/win32/opengl32/pixels.c:140) honors that setting. For alignment 1, row length 64, and an 8×2 bitmap, compilation retains two bytes; replay reads the second row at byte eight.
   
   **Smallest fix:** share one checked bitmap-layout calculation between capture and execution, including row length and skips. Add an ASan regression with caller-data mutation after compilation.

3. **P1 — Fullscreen → windowed presentation retains the exclusive lease.**  
   [present.c:169](sdk/win32/opengl32/present.c:169) enters the GDI path without releasing `c->leased_window`. Changing the existing popup into an overlapped window therefore leaves composition suspended by [compositor.c:1067](apps/desktop/compositor.c:1067). After finding 1 is fixed, its completed-present RPC times out because no compositor frame can complete.
   
   **Smallest fix:** synchronously release the context’s retained lease before entering windowed presentation. Test fullscreen → windowed on the same HWND/DC/context.

4. **P1 — Compatibility scaling crops normal application viewports.**  
   [context.c:126](sdk/win32/opengl32/context.c:126) shrinks the allocation, while [engine.c:1324](sdk/win32/opengl32/engine.c:1324) retains application viewport coordinates unchanged. A 640×480 application using the 320×240 setting draws into a 640×480 viewport over a 320×240 buffer; presentation stretches the surviving portion. Allocation dimensions also never follow drawable resize or rebinding to another compatible DC.
   
   **Smallest fix:** retain drawable and render dimensions separately, map window-coordinate operations consistently, and refresh backing storage when the drawable changes. Add scaled viewport/scissor/readback and resize cases.

5. **P1 — Rendering on another thread silently loses the window presentation path.**  
   [paint.c:583](sdk/win32/user32/paint.c:583) rejects a target query outside the HWND’s owner thread. [present.c:44](sdk/win32/opengl32/present.c:44) then treats that window DC as a generic GDI target, leaves `window=0`, and skips completed-present submission. WGL permits moving a released context between threads; the current host test checks only simultaneous ownership refusal. [Microsoft’s context rules](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-wglmakecurrent) support that distinction.
   
   **Smallest fix:** preserve window identity for worker rendering and marshal presentation through its owner, or explicitly reject that binding instead of reporting successful swaps.

6. **P2 — Projective texture coordinates are divided too early.**  
   [engine.c:36](sdk/win32/opengl32/engine.c:36) divides S/T by texture Q at each vertex. The rasterizer subsequently interpolates those values using geometric reciprocal-W. Varying Q requires interpolating S/W, T/W and Q/W before the final division; the present calculation distorts projective textures and generated Q coordinates.
   
   **Smallest fix:** retain a per-unit Q/W interpolant and divide after interpolation. Add varying-Q and varying-W oracles against the [GL 1.1 interpolation rules](https://registry.khronos.org/OpenGL/specs/gl/glspec11.pdf).

7. **P2 — Post-divide UV narrowing violates the stated overflow bounds.**  
   [renderer.c:371](sdk/libgl/software/renderer.c:371) narrows a potentially large 64-bit quotient to `int`. Accepted inputs such as W=100 and S=40000 exceed signed 16.16 range after reconstruction; on the target this can turn a positive clamp-to-edge coordinate negative and sample the opposite edge.
   
   **Smallest fix:** perform wrap/clamp reduction in wide arithmetic before narrowing, and test the extrema of the accepted W/UV domain.

8. **P2 — Mip selection depends on vertex ordering and never produces fractional LOD.**  
   [renderer.c:340](sdk/libgl/software/renderer.c:340) estimates footprint from only edge A–B and a bounding-box span. Cyclically rotating the same triangle can change its mip. It also measures UV/W without reconstructing texture-space derivatives and produces only multiples of 256, leaving trilinear interpolation’s fractional weight zero.
   
   **Smallest fix:** derive order-independent screen-space gradients with reciprocal-W correction and retain fractional LOD. Test cyclic permutations, strong perspective and differently colored mip levels.

9. **P2 — Flat shading chooses colors after primitive splitting/clipping.**  
   [geometry.c:110](sdk/win32/opengl32/geometry.c:110) selects each delivered triangle’s third color. TinyGL splits a quad into triangles ending at different original vertices, so a flat-shaded quad can show two colors. Homogeneous clipping can likewise replace the original provoking vertex before this callback.
   
   **Smallest fix:** preserve the original primitive’s flat color before triangulation and clipping, then propagate it to every generated triangle.

10. **P2 — Texture subimages bypass internal-format conversion.**  
    [texture.c:157](sdk/win32/opengl32/texture.c:157) stores decoded source RGBA directly, unlike initial image upload. Updating a luminance texture with colored RGB data therefore introduces colored samples; updating intensity fails to replicate intensity into alpha.
    
    **Smallest fix:** reuse the initial upload’s internal-format conversion for subimages. Test upload/subimage equivalence for each implemented base format.

11. **P2 — Polygon depth offset leaks into unrelated primitives and pixels.**  
    [engine.c:86](sdk/win32/opengl32/engine.c:86) leaves the last triangle’s offset in shared raster state. [renderer.c:87](sdk/libgl/software/renderer.c:87) applies it universally; subsequent lines, points, bitmaps and drawn pixels inherit that offset, including after disabling polygon offset.
    
    **Smallest fix:** make the offset specific to the current primitive and clear it for pixel operations. Add an offset-triangle → unoffset-line/pixel depth regression.

12. **P2 — The advertised compression extension rejects a required valid hint.**  
    [validate.c:385](sdk/win32/opengl32/validate.c:385) rejects `GL_TEXTURE_COMPRESSION_HINT_ARB`, and its state is absent from queries. Yet `GL_ARB_texture_compression` is advertised. Its [specification](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_texture_compression.txt) defines both the hint and query.
    
    **Smallest fix:** implement its validated, queryable hint state and test it before retaining the extension string.

13. **P2 — Fixture records do not measure the claimed sizes or completed frames.**  
    [exercise.c:80](src/probes/win32_gl/exercise.c:80) increments frames even when swapping fails; lines 89–90 hard-code 320×240. The requested 352×288 overlapped window has a different client size, which determines the actual allocation without a scaling shim. Meanwhile, [f5-core.json:1391](tests/suites/f5-core.json:1391) accepts any nonzero checksum, including a nonzero hash of an incorrectly blank scene.
    
    **Smallest fix:** count successful completions, report actual allocation/display dimensions, and validate guest scene content against qualified expectations. Add transport/compositor failure, client-death, lease-transition and scaling coverage using production paths.

**Merge verdict: request changes.** Fix the P1 failures and bitmap memory-safety defect first, then close the rendering/extension correctness findings. Successful sanitizer qualification and integration evidence remain required; the existing host goldens do not establish this delivery’s presentation correctness.