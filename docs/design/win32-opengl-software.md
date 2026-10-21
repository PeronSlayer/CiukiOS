# Win32 software OpenGL (f7-07)

## Amendment 3 cross-review decisions (2026-10-21)

Primary sources rechecked before the fixes:
[GL 1.1](https://registry.khronos.org/OpenGL/specs/gl/glspec11.pdf),
§§2.13.7–8, 3.5–3.8 and 4.3 (provoking colors before clipping,
projective S/W, T/W, Q/W interpolation, texture derivatives, bitmap unpack
layout, internal image formats, and pixel coordinates);
[ARB_texture_compression](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_texture_compression.txt)
(the queryable compression hint, initially DONT_CARE);
[wglMakeCurrent](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-wglmakecurrent)
(exclusive current-thread ownership and compatible drawable rebinding), and
[SetPixelFormat](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-setpixelformat)
(window lifetime). Wine's retained declarations remain ABI references only.

The reviewed implementation loses PRESENT geometry before transport, captures
bitmaps with a different pitch from replay, retains an exclusive lease on the
windowed path, and confuses drawable coordinates with the scaled allocation.
Correct those paths and qualify them through production transport/presentation
host adapters. Keep drawable and render sizes separate; application viewport,
scissor, raster and readback coordinates stay in drawable space and map to the
backing allocation. Refresh that allocation on resize and compatible rebinding.
USER's owner-thread transport cannot marshal GL swaps from workers today:
explicitly reject window-context binding from a different thread, including a
released context, rather than let a window DC fall through to generic GDI.
Memory DC contexts retain released-context migration between threads.

Keep texture coordinates and mip gradients wide until wrap/clamp reduction;
interpolate per-unit Q/W and calculate perspective-correct, order-independent
screen derivatives with fractional LOD. Preserve primitive flat color before
splitting/clipping, reuse image-format conversion for subimages, and scope
depth offset to each polygon operation. Guest scene expectations must be
qualified against exact host checksums; dimensions and frame totals must
come from actual allocation and successful completed swaps. Host evidence
does not qualify guest compositor timing or hardware.

The retained fragment representation bounds S/W and T/W to ±2048 (16
fractional bits) and Q/W to ±128 (20 fractional bits), so every stored
interpolant has magnitude at most 2^27. W retains its existing positive
transform bounds. Barycentric accumulation and reconstructed UV use signed
64-bit arithmetic; repeat/clamp reduction precedes any narrowing. Integer
quotient-rule gradients use the maximum absolute screen derivative as the
texture footprint approximation. A bounded binary logarithm retains eight
fractional LOD bits; very large footprints saturate at the largest supported
mip. Identical min/mag filters need no LOD calculation.

`CiukiGLGetSizes` is a value-only diagnostic export of opengl32: six DWORDs
containing render, drawable and physical display width/height pairs for the
current context/DC. It adds no advertised GL extension. The fixture uses it
in the version-2 result block, retaining its 144-byte size and result address.
It requests an exact 320×240 client via AdjustWindowRect, and both the
controller and suite require the seven unchanged, host-qualified checksums.
The guest case remains authored and unrun. Host tests exercise the actual
fixture body (including failed swaps), USER's PRESENT RPC and target query,
du3_request, server receipt, compositor failure/retry, lease restoration and
client teardown. Transport/compositor tests substitute kernel value boundaries.
The fixture-body replay supplies deterministic USER startup, font-success and
process-exit boundaries; production glyph conversion is qualified separately.

## Primary sources and repository comparison

Research, 2026-10-20; primary sources rechecked 2026-10-21:

- [OpenGL 1.1 specification](https://registry.khronos.org/OpenGL/specs/gl/glspec11.pdf),
  sections 2.5–2.13, 3.6–3.8, 4.1–4.3 and 5.4: errors, transformations,
  lighting, pixel storage, texture environments, fragment ordering and lists.
- [Microsoft rendering contexts](https://learn.microsoft.com/en-us/windows/win32/opengl/rendering-context-functions):
  current contexts belong to threads; a context cannot be current on two threads.
- [PIXELFORMATDESCRIPTOR](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-pixelformatdescriptor)
  and [SetPixelFormat](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-setpixelformat):
  the descriptor is 40 bytes, format indices start at one, and a window's
  format cannot change after selection. Software formats report GENERIC_FORMAT.
- [wglGetProcAddress](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-wglgetprocaddress):
  unknown names and lookup without a current context fail.
- [ARB multitexture](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_multitexture.txt)
  and [S3TC](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_texture_compression_s3tc.txt):
  independent texture units and block upload decoding are software facilities.
- [Wine declarations](https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/opengl32/opengl32.spec)
  (the local Wine 10.0 `include/wine/wgl.h` and `opengl32.spec`): ABI shapes
  only; no Wine renderer implementation is used.

The native TinyGL port has one global context, fixed 256-square RGB textures,
fixed depth comparison, and no mipmaps, alpha test, scissor or standard GL
extension strings. Its native surface adapter and goldens remain a separate
qualification. Win32 needs context ownership, explicit error/capability
reporting and a private back buffer before GDI presentation. New raster state
must not silently change the already qualified native scenes.

The compatibility path keeps TinyGL's CPU transformation, lighting, clipping
with opt-in triangle/line callbacks. Its fragment backend
uses bounded integer edge/interpolation loops, 16-bit depth and XRGB colour.
Amendment 1 permits the scalar x87 transform/lighting ABI in user processes;
the kernel presenter and fragment backend remain integer and SIMD-free.
No vendor registers are accessed. This is a software subset, without a full
GL conformance claim; evidence is recorded in `build/codex/f7-07-report.md`.

## Continuation and implemented boundaries (2026-10-21)

The interrupted WIP did not contain a renderer DLL or a fixture. Its texture
cleanup attempted to free inline TinyGL pixel arrays; those frees were
removed. The retained packed-pixel enum values were also corrected. Vendor
changes are identified and hashed in `sdk/libgl/port.json`.

Additional primary references checked for this implementation:

- [OpenGL 1.2.1](https://registry.khronos.org/OpenGL/specs/gl/glspec121.pdf),
  the normative replacement referenced by the ARB_multitexture registry entry.
- [ARB_texture_env_combine](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_texture_env_combine.txt):
  the six combine operations, RGB/alpha sources and operands, and 1/2/4 scales.
- [ARB_texture_compression](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_texture_compression.txt):
  compressed upload, subimage, size/format queries and retrieval.
- [EXT_compiled_vertex_array](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_compiled_vertex_array.txt):
  the implementation may decline caching; lock/unlock preserve client-array
  behavior and expose their first/count state.
- [EXT_bgra](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_bgra.txt) and
  [EXT_texture_env_add](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_texture_env_add.txt):
  BGR/BGRA component order and saturated RGB addition with format-dependent alpha.
- [wglUseFontBitmaps](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-wglusefontbitmapsa):
  each glyph becomes one bitmap display list. GDI's selected font provider
  (including the existing service font bridge) supplies owned coverage; GL
  retains a packed bottom-up copy, bearing and advance.
- [wglShareLists](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-wglsharelists)
  and [SwapBuffers](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-swapbuffers):
  compatible contexts share objects; swapping has a GDI entry point.

The Win32 adapter retains TinyGL transformation, lighting and homogeneous
clipping. It uses its own bounded, copied display-list commands, rather than
TinyGL lists, so extension state and bitmap ownership follow the same path.
`sdk/libgl/software` supplies integer fragment/depth/blend/texture operations;
the native SDK archive includes this backend without changing the native
adapter's default renderer. It provides XRGB color storage, RGB565
quantization for 16-bit formats, and D16 depth. Four software pixel formats
report zero stencil, accumulation and destination-alpha bits. ABI forwarding
is generated from the retained 336 GL 1.1 declarations and extension shapes;
the export set is not a full GL conformance claim.

Contexts are serialized around TinyGL's selected context; ownership is per
thread. Share groups retain software texture images and copied display-list
commands until their final context is deleted. Two active/client units and
two texture-matrix stacks are distinct. The rendering buffer is a GDI DIB
section. Existing `ddraw-scale` compatibility settings can select 320x240,
400x300, 512x384 or 640x480; size records name the actual render/display sizes.
Fullscreen popup windows obtain the existing USER display lease, submit the
exact token/generation to `W32_DISPLAY_FRAME`, and count only successful
completion. Conversion to RGB565 occurs before a 16-bit lease submission.

For windowed GL, `DESK_V3_PRESENT` has the same bounded 16-byte damage body
as `DESK_DAMAGE`. It requires a nonzero request, window id and generation,
permits no attachments, and allows one outstanding request per window. USER
waits through its existing bounded RPC. The desktop queues its matching
`DESK_V3_RESULT` only after `desk_redraw` completes successfully. Failed
frames retain the request for retry; minimized/collapsed requests fail.
The completion helper has a host test for identity, bounds, attachment
refusal and duplicate completion. Guest timing, revocation and mode-change
qualification remain the lead's work.

The authored fixture and observer share a 144-byte copied result block at
`0x100e0000` (Amendment 2; `0x10040000` belongs to fullscreen). The fixture
reserves that page before creating a window. Seven
scene checksums, frame counts, format/extension counts and glyph-list status
are collected by the kernel from its verified Win32 desktop child. The
selector, loader table, runner model and `f5-core.json` case are registered;
the guest case is authored and unrun.

### Beta boundary

Amendment 1 permits scalar x87 in user-space transforms and lighting; the
fragment backend and kernel presenter remain integer and SIMD-free. The
336 exported GL entry points describe an ABI, not full GL conformance.
Evaluators, selection/feedback, accumulation and stencil operations report
errors. Pixel maps/transfer, non-byte pixel types (except retained RGB565),
texture borders and full front/back draw/read-buffer semantics remain outside
this beta subset; no corresponding extension is advertised. The software
renderer has no hardware acceleration claim.

The v3 completed-present integration review and guest/hardware qualification
remain with the lead. The new GL sanitized suite keeps leak detection enabled;
its host sandbox cannot complete LeakSanitizer's ptrace-based teardown.
The final host commands and precise gate status are in
`build/codex/f7-07-report.md`.

## Amendment 1 implementation decisions (2026-10-21)

The lead permits scalar x87 in user transforms/lighting; the kernel presenter
and integer fragment backend retain their SIMD-free restrictions. The minimal
versioned v3 completion request remains subject to integration review.

Before implementation, the sources above were rechecked: GL 1.1 §§2.10.4,
2.11, 3.8.4 and 5.4 specify transformed eye planes, independent texture targets,
retained bindings in other sharing contexts, and compile-time pointer capture.
Microsoft SetPixelFormat requires window lifetime persistence. The WIP lacked
these paths. Implement six user planes and per-unit texgen, separate bindings
and default objects, reference-counted deleted textures, window-owned format
storage, byte-format pixel oracles and a common display-list validation path.
Evaluators, selection/feedback, accumulation and stencil remain error-returning
beta exclusions. Host tests qualify the advertised extensions; guest evidence
and the sanitized acceptance rerun belong to the lead.

The window-format slot uses a v2 USER/GDI table tail; v1 tables retain their
original prefix and are never read past it. The production USER fixture
checks an exact-size v1 table under ASan as well as the v2 window/DC path.
The final host evidence has seven unchanged fixture goldens and eleven new
amendment goldens, with separate pixel/region/reflection oracles.

## Amendment 2 integration decisions (2026-10-21)

Rechecked the primary GL 1.1 specification (especially §5.5), Microsoft's
SetPixelFormat, PIXELFORMATDESCRIPTOR,
[wglMakeCurrent](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-wglmakecurrent), wglGetProcAddress and
SwapBuffers documentation linked above, and Wine's upstream opengl32.spec
declarations before resolving the merge. The window-owned pixel-format and
per-thread context rules, software format flags and ABI shapes remain as
implemented in Amendment 1. The internal completed-present request is a
CiukiOS transport decision; it does not claim that Microsoft requires display
scanout completion on SwapBuffers. No renderer code is imported from Wine.

Main's shared `GATE_W32_FIXTURE` now launches `gl-exercise` after a successful
registry OPEN and CLOSE, with the same bounded readiness and child-report
authentication as its existing fixtures. The ten core schema IDs and restart
ID are preserved; GL has a separate launcher ID and keeps its own result
protocol. The old GL-specific gate and dialogs launcher mode are removed.
The observer reads `0x100e0000`, leaving main's pages through `0x100d0000`
unchanged, and uses the shared descendant cleanup that stops desktop
supervision before collecting children. All three selector tables retain
main's probes and register `w32-gl` exactly once. The versioned v3
completed-present helper remains linked alongside the shared fixture gate.

The GL suite remains authored and unrun. Host verification commands, results
and any sanitizer-environment failures are recorded under Amendment 2 in
`build/codex/f7-07-report.md`; they do not replace guest evidence or the lead's
integration review.

Canonical-flags compilation of the merged desktop probe also exposed a
4120-byte `restart_link` frame from main's 4096-byte local shortcut buffer.
[Clang's frame-size diagnostic](https://clang.llvm.org/docs/DiagnosticsReference.html#wframe-larger-than)
and [C11 draft N1570 §6.2.4](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf)
were checked. Keep the 4096-byte frame limit and buffer capacity; give this
single-controller probe's scratch buffer static storage duration. The restart
observer already has shared per-run state and does not run concurrently with
another selected probe. This adds 4096 bytes of zero-initialized kernel
storage, without allocation, new failure paths or changes to shortcut parsing.
